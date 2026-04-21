#include "switchbox/core/switch_mpv_player.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)
#include <deko3d.hpp>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_dk3d.h>
#include <mpv/stream_cb.h>
#endif

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/core/smb2_mount_fs.hpp"

namespace switchbox::core {

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)

namespace {

constexpr bool kPlaybackDebugLogEnabled = false;

struct PlaybackDebugLogState {
    std::mutex mutex;
    bool initialized = false;
    uint64_t sequence = 0;
    uint64_t session_token = 0;
    std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::time_point::min();
};

PlaybackDebugLogState& playback_debug_log_state() {
    static PlaybackDebugLogState state;
    return state;
}

std::filesystem::path playback_debug_log_path() {
    return switchbox::core::AppConfigStore::paths().base_directory / "playback_debug.log";
}

uint64_t make_playback_debug_session_token() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto token = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return token == 0 ? 1 : token;
}

std::string sanitize_debug_log_message(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    return value;
}

void append_debug_log_locked(PlaybackDebugLogState& state, const std::string& message) {
    if (!kPlaybackDebugLogEnabled) {
        return;
    }

    std::ofstream output(playback_debug_log_path(), std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (state.session_start == std::chrono::steady_clock::time_point::min()) {
        state.session_start = now;
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.session_start).count();
    output << "[t+" << elapsed_ms << "ms]"
           << "[#" << (++state.sequence) << "] "
           << sanitize_debug_log_message(message)
           << '\n';
}

void reset_debug_log_session_locked(PlaybackDebugLogState& state) {
    if (!kPlaybackDebugLogEnabled) {
        return;
    }

    std::error_code error;
    const auto log_path = playback_debug_log_path();
    std::filesystem::create_directories(log_path.parent_path(), error);

    std::ofstream output(log_path, std::ios::binary | std::ios::out | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    state.initialized = true;
    state.session_start = std::chrono::steady_clock::now();
    state.session_token = make_playback_debug_session_token();

    output << "\n========== PLAYBACK LOG SESSION BEGIN ==========\n";
    output << "[t+0ms][#" << (++state.sequence) << "] [session] log_format=2026-04-17f\n";
    output << "[t+0ms][#" << (++state.sequence) << "] [session] session_token=" << state.session_token << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [session] base_directory="
           << switchbox::core::AppConfigStore::paths().base_directory.string()
           << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [session] config_file="
           << switchbox::core::AppConfigStore::paths().config_file.string()
           << '\n';
    output.flush();
}

void begin_debug_log_session_impl() {
    if (!kPlaybackDebugLogEnabled) {
        return;
    }

    auto& state = playback_debug_log_state();
    std::scoped_lock lock(state.mutex);
    reset_debug_log_session_locked(state);
}

void append_debug_log(const std::string& message) {
    if (!kPlaybackDebugLogEnabled) {
        return;
    }

    auto& state = playback_debug_log_state();

    std::scoped_lock lock(state.mutex);
    if (!state.initialized) {
        reset_debug_log_session_locked(state);
    }
    append_debug_log_locked(state, message);
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

constexpr const char* kEmbeddedSubtitleFontDirectory = "romfs:/font";
constexpr const char* kEmbeddedSubtitleFontFamily = "Droid Sans Fallback";
constexpr uint64_t kInitialLoadTimeoutMs = 15000;
constexpr uint64_t kIptvInitialLoadTimeoutMs = 45000;
constexpr const char* kMemoryTextProtocol = "switchboxmem";
constexpr const char* kMemoryTextProtocolPrefix = "switchboxmem://";

struct MemoryTextStreamData {
    std::vector<char> bytes;
};

struct MemoryTextStreamCookie {
    std::string key;
    std::shared_ptr<MemoryTextStreamData> data;
    uint64_t position = 0;
    std::atomic_bool cancelled = false;
};

struct MemoryTextStreamRegistry {
    std::mutex mutex;
    uint64_t next_id = 0;
    std::unordered_map<std::string, std::shared_ptr<MemoryTextStreamData>> streams;
};

void configure_embedded_subtitle_fonts(mpv_handle* handle) {
    if (handle == nullptr) {
        return;
    }

    const std::string force_style = std::string("FontName=") + kEmbeddedSubtitleFontFamily;
    mpv_set_option_string(handle, "sub-fonts-dir", kEmbeddedSubtitleFontDirectory);
    mpv_set_option_string(handle, "sub-font", kEmbeddedSubtitleFontFamily);
    mpv_set_option_string(handle, "osd-font", kEmbeddedSubtitleFontFamily);
    mpv_set_option_string(handle, "sub-ass-force-style", force_style.c_str());
}

uint64_t fnv1a64(std::string_view value) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string to_hex(uint64_t value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int index = 15; index >= 0; --index) {
        result[static_cast<size_t>(index)] = hex[value & 0x0F];
        value >>= 4;
    }
    return result;
}

MemoryTextStreamRegistry& memory_text_stream_registry() {
    static MemoryTextStreamRegistry registry;
    return registry;
}

std::string sanitize_memory_text_extension(std::string extension) {
    extension = trim(std::move(extension));
    while (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }

    std::string sanitized;
    sanitized.reserve(extension.size());
    for (char& character : extension) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (std::isalnum(unsigned_character) != 0 || character == '-' || character == '_') {
            sanitized.push_back(
                unsigned_character < 0x80 ? static_cast<char>(std::tolower(unsigned_character)) : character);
        }
    }

    return sanitized.empty() ? "txt" : sanitized;
}

bool is_memory_text_locator(std::string_view locator) {
    return locator.rfind(kMemoryTextProtocolPrefix, 0) == 0;
}

bool is_inline_data_locator(std::string_view locator) {
    std::string lower(locator);
    for (char& character : lower) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return lower.rfind("data://", 0) == 0;
}

bool is_builtin_memory_locator(std::string_view locator) {
    std::string lower(locator);
    for (char& character : lower) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return lower.rfind("memory://", 0) == 0 || lower.rfind("hex://", 0) == 0;
}

bool is_edl_locator(std::string_view locator) {
    std::string lower(locator);
    for (char& character : lower) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return lower.rfind("edl://", 0) == 0;
}

std::string summarize_locator_for_debug_log(std::string_view locator) {
    constexpr size_t kMaxVisibleLength = 240;

    if (is_inline_data_locator(locator)) {
        const std::string value(locator);
        const size_t comma = value.find(',');
        const std::string prefix =
            comma == std::string::npos ? value : value.substr(0, comma);
        return prefix + ",...(len=" + std::to_string(value.size()) + ")";
    }

    if (is_builtin_memory_locator(locator)) {
        std::string value(locator);
        const size_t separator = value.find("://");
        const std::string prefix =
            separator == std::string::npos ? value : value.substr(0, separator + 3);
        return prefix + "...(len=" + std::to_string(value.size()) + ")";
    }

    if (is_edl_locator(locator)) {
        return "edl://...(len=" + std::to_string(locator.size()) + ")";
    }

    std::string value(locator);
    if (value.size() <= kMaxVisibleLength) {
        return value;
    }

    return value.substr(0, kMaxVisibleLength) + "...(len=" + std::to_string(value.size()) + ")";
}

std::string extract_memory_text_key(std::string_view uri) {
    if (!is_memory_text_locator(uri)) {
        return {};
    }

    return std::string(uri.substr(std::char_traits<char>::length(kMemoryTextProtocolPrefix)));
}

int64_t memory_text_stream_read(void* cookie_ptr, char* buffer, uint64_t byte_count) {
    auto* cookie = static_cast<MemoryTextStreamCookie*>(cookie_ptr);
    if (cookie == nullptr || cookie->data == nullptr || buffer == nullptr) {
        return -1;
    }
    if (cookie->cancelled.load()) {
        return -1;
    }

    const uint64_t size = cookie->data->bytes.size();
    if (cookie->position >= size) {
        return 0;
    }

    const uint64_t remaining = size - cookie->position;
    const uint64_t to_copy = std::min(byte_count, remaining);
    std::memcpy(
        buffer,
        cookie->data->bytes.data() + static_cast<size_t>(cookie->position),
        static_cast<size_t>(to_copy));
    cookie->position += to_copy;
    return static_cast<int64_t>(to_copy);
}

int64_t memory_text_stream_seek(void* cookie_ptr, int64_t offset) {
    auto* cookie = static_cast<MemoryTextStreamCookie*>(cookie_ptr);
    if (cookie == nullptr || cookie->data == nullptr) {
        return MPV_ERROR_GENERIC;
    }
    if (cookie->cancelled.load()) {
        return MPV_ERROR_GENERIC;
    }
    if (offset < 0 || static_cast<uint64_t>(offset) > cookie->data->bytes.size()) {
        return MPV_ERROR_GENERIC;
    }

    cookie->position = static_cast<uint64_t>(offset);
    return offset;
}

int64_t memory_text_stream_size(void* cookie_ptr) {
    auto* cookie = static_cast<MemoryTextStreamCookie*>(cookie_ptr);
    if (cookie == nullptr || cookie->data == nullptr) {
        return MPV_ERROR_UNSUPPORTED;
    }

    return static_cast<int64_t>(cookie->data->bytes.size());
}

void memory_text_stream_cancel(void* cookie_ptr) {
    auto* cookie = static_cast<MemoryTextStreamCookie*>(cookie_ptr);
    if (cookie != nullptr) {
        cookie->cancelled.store(true);
    }
}

void memory_text_stream_close(void* cookie_ptr) {
    std::unique_ptr<MemoryTextStreamCookie> cookie(static_cast<MemoryTextStreamCookie*>(cookie_ptr));
}

int memory_text_stream_open_ro(void*, char* uri, mpv_stream_cb_info* info) {
    if (uri == nullptr || info == nullptr) {
        return MPV_ERROR_LOADING_FAILED;
    }

    const std::string key = extract_memory_text_key(uri);
    if (key.empty()) {
        return MPV_ERROR_LOADING_FAILED;
    }

    auto& registry = memory_text_stream_registry();
    std::shared_ptr<MemoryTextStreamData> data;
    {
        std::scoped_lock lock(registry.mutex);
        const auto iterator = registry.streams.find(key);
        if (iterator == registry.streams.end()) {
            return MPV_ERROR_LOADING_FAILED;
        }
        data = iterator->second;
    }

    auto* cookie = new (std::nothrow) MemoryTextStreamCookie();
    if (cookie == nullptr) {
        return MPV_ERROR_GENERIC;
    }

    cookie->key = key;
    cookie->data = std::move(data);
    info->cookie = cookie;
    info->read_fn = &memory_text_stream_read;
    info->seek_fn = &memory_text_stream_seek;
    info->size_fn = &memory_text_stream_size;
    info->close_fn = &memory_text_stream_close;
    info->cancel_fn = &memory_text_stream_cancel;
    return 0;
}

const mpv_node* node_map_find(const mpv_node_list* map, const char* key) {
    if (map == nullptr || key == nullptr) {
        return nullptr;
    }

    for (int index = 0; index < map->num; ++index) {
        if (map->keys == nullptr || map->values == nullptr || map->keys[index] == nullptr) {
            continue;
        }
        if (std::strcmp(map->keys[index], key) == 0) {
            return &map->values[index];
        }
    }

    return nullptr;
}

std::string node_to_string(const mpv_node* node) {
    if (node == nullptr) {
        return {};
    }

    if (node->format == MPV_FORMAT_STRING && node->u.string != nullptr) {
        return node->u.string;
    }

    return {};
}

bool node_to_bool(const mpv_node* node, bool fallback = false) {
    if (node == nullptr) {
        return fallback;
    }

    if (node->format == MPV_FORMAT_FLAG) {
        return node->u.flag != 0;
    }

    if (node->format == MPV_FORMAT_INT64) {
        return node->u.int64 != 0;
    }

    return fallback;
}

bool node_to_int(const mpv_node* node, int& output) {
    if (node == nullptr) {
        return false;
    }

    if (node->format == MPV_FORMAT_INT64) {
        output = static_cast<int>(node->u.int64);
        return true;
    }

    return false;
}

std::string build_track_label(const mpv_node_list* map, int id) {
    const std::string title = trim(node_to_string(node_map_find(map, "title")));
    const std::string lang = trim(node_to_string(node_map_find(map, "lang")));

    if (!title.empty() && !lang.empty()) {
        return title + " (" + lang + ")";
    }
    if (!title.empty()) {
        return title;
    }
    if (!lang.empty()) {
        return lang;
    }
    return "Track " + std::to_string(id);
}

std::string pick_locator(const PlaybackTarget& target) {
    if (!target.primary_locator.empty()) {
        return target.primary_locator;
    }

    return target.fallback_locator;
}

bool is_http_locator(std::string_view locator) {
    std::string lower(locator);
    for (char& character : lower) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool locator_looks_like_hls_playlist(std::string_view locator) {
    std::string lower(locator);
    for (char& character : lower) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }

    const size_t query_begin = lower.find_first_of("?#");
    if (query_begin != std::string::npos) {
        lower.resize(query_begin);
    }

    return lower.size() >= 5 && lower.ends_with(".m3u8");
}

std::string build_default_iptv_user_agent() {
    // Match the proven UA used by nxmp/libmpv on Switch to reduce the chance
    // of source-specific server gating on unusual client identifiers.
    return "Mozilla/5.0 (X11; Linux x86_64; rv:49.0) Gecko/20100101 Firefox/49.0";
}

std::string join_http_header_fields_for_mpv(const std::vector<std::string>& fields) {
    std::string result;
    for (const std::string& field : fields) {
        if (field.empty()) {
            continue;
        }

        if (!result.empty()) {
            result += ",";
        }
        result += field;
    }
    return result;
}

std::string join_option_keys_for_debug_log(const std::vector<std::pair<std::string, std::string>>& options) {
    std::string result;
    for (const auto& option : options) {
        if (option.first.empty()) {
            continue;
        }

        if (!result.empty()) {
            result += ",";
        }
        result += option.first;
    }
    return result;
}

void append_lavf_key_value(std::string& destination, std::string_view key, std::string value) {
    value = trim(std::move(value));
    if (value.empty()) {
        return;
    }

    if (value.find(',') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) {
        return;
    }

    if (!destination.empty()) {
        destination += ",";
    }
    destination += std::string(key);
    destination += "=";
    destination += value;
}

void set_option_pair(
    std::vector<std::pair<std::string, std::string>>& options,
    std::string key,
    std::string value) {
    key = trim(std::move(key));
    value = trim(std::move(value));
    if (key.empty() || value.empty()) {
        return;
    }

    for (auto& option : options) {
        if (option.first == key) {
            option.second = std::move(value);
            return;
        }
    }

    options.emplace_back(std::move(key), std::move(value));
}

std::vector<std::pair<std::string, std::string>> build_iptv_open_options(
    const PlaybackTarget& target,
    const std::string& locator) {
    std::vector<std::pair<std::string, std::string>> options;
    if (target.source_kind != PlaybackSourceKind::Iptv) {
        return options;
    }

    if (target.iptv_open_plan.has_value() && !target.iptv_open_plan->option_pairs.empty()) {
        return target.iptv_open_plan->option_pairs;
    }

    const bool locator_is_http = is_http_locator(locator);
    const bool locator_is_hls_http = locator_is_http && locator_looks_like_hls_playlist(locator);
    const bool locator_is_memory_text = is_memory_text_locator(locator);
    const bool locator_is_inline_data = is_inline_data_locator(locator);
    const bool locator_is_builtin_memory = is_builtin_memory_locator(locator);
    const bool locator_is_edl = is_edl_locator(locator);
    const std::string user_agent =
        trim(target.http_user_agent).empty() ? build_default_iptv_user_agent() : trim(target.http_user_agent);
    const std::string referrer = trim(target.http_referrer);

    std::vector<std::string> header_fields = target.http_header_fields;
    std::string demuxer_lavf_options = "http_persistent=1,http_multiple=1,tls_verify=0";
    append_lavf_key_value(demuxer_lavf_options, "user_agent", user_agent);
    append_lavf_key_value(demuxer_lavf_options, "referer", referrer);

    if (locator_is_http || locator_is_memory_text || locator_is_builtin_memory || locator_is_edl) {
        set_option_pair(options, "user-agent", user_agent);
        set_option_pair(options, "tls-verify", "no");
        set_option_pair(options, "network-timeout", "45");
        set_option_pair(
            options,
            "stream-lavf-o",
            "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=3,"
            "multiple_requests=1,tls_verify=0");
        if (!referrer.empty()) {
            set_option_pair(options, "referrer", referrer);
        }

        const std::string joined_headers = join_http_header_fields_for_mpv(header_fields);
        if (!joined_headers.empty()) {
            set_option_pair(options, "http-header-fields", joined_headers);
        }
    }

    if (locator_is_hls_http || locator_is_memory_text || locator_is_inline_data || locator_is_builtin_memory) {
        // In-memory HLS playlists are already rewritten to absolute child URLs,
        // so mpv should bypass its generic playlist parser and hand them
        // directly to FFmpeg's HLS demuxer. We apply the same hint to direct
        // .m3u8 IPTV URLs because some servers expose non-standard MIME types
        // and Switch libmpv otherwise stalls before FILE_LOADED.
        set_option_pair(options, "demuxer", "+lavf");
        set_option_pair(options, "demuxer-lavf-format", "hls");
    }

    set_option_pair(options, "demuxer-lavf-o", demuxer_lavf_options);

    return options;
}

uint64_t initial_load_timeout_ms_for_source(PlaybackSourceKind source_kind) {
    switch (source_kind) {
        case PlaybackSourceKind::Iptv:
            return kIptvInitialLoadTimeoutMs;
        case PlaybackSourceKind::Smb:
        case PlaybackSourceKind::Unknown:
        default:
            return kInitialLoadTimeoutMs;
    }
}

enum class AsyncCommandKind : uint64_t {
    Unknown = 0,
    LoadFile = 1,
    Stop = 2,
    Seek = 3,
};

AsyncCommandKind async_command_kind_from_reply_userdata(uint64_t reply_userdata) {
    switch (reply_userdata) {
        case static_cast<uint64_t>(AsyncCommandKind::LoadFile):
            return AsyncCommandKind::LoadFile;
        case static_cast<uint64_t>(AsyncCommandKind::Stop):
            return AsyncCommandKind::Stop;
        case static_cast<uint64_t>(AsyncCommandKind::Seek):
            return AsyncCommandKind::Seek;
        default:
            return AsyncCommandKind::Unknown;
    }
}

const char* async_command_kind_label(AsyncCommandKind kind) {
    switch (kind) {
        case AsyncCommandKind::LoadFile:
            return "loadfile";
        case AsyncCommandKind::Stop:
            return "stop";
        case AsyncCommandKind::Seek:
            return "seek";
        case AsyncCommandKind::Unknown:
        default:
            return "unknown";
    }
}

int command_loadfile_with_options(
    mpv_handle* handle,
    const std::string& locator,
    const std::vector<std::pair<std::string, std::string>>& option_pairs) {
    if (handle == nullptr) {
        return MPV_ERROR_INVALID_PARAMETER;
    }

    if (option_pairs.empty()) {
        const char* command[] = {"loadfile", locator.c_str(), "replace", nullptr};
        return mpv_command_async(
            handle,
            static_cast<uint64_t>(AsyncCommandKind::LoadFile),
            command);
    }

    std::string command_name = "loadfile";
    std::string replace_mode = "replace";
    std::vector<std::pair<std::string, std::string>> mutable_options = option_pairs;
    std::vector<char*> option_keys;
    std::vector<mpv_node> option_values;
    option_keys.reserve(mutable_options.size());
    option_values.reserve(mutable_options.size());

    for (auto& option : mutable_options) {
        option_keys.push_back(option.first.data());
        mpv_node value {};
        value.format = MPV_FORMAT_STRING;
        value.u.string = option.second.data();
        option_values.push_back(value);
    }

    mpv_node_list option_map {};
    option_map.num = static_cast<int>(mutable_options.size());
    option_map.keys = option_keys.data();
    option_map.values = option_values.data();

    std::array<mpv_node, 4> arguments {};
    arguments[0].format = MPV_FORMAT_STRING;
    arguments[0].u.string = command_name.data();
    arguments[1].format = MPV_FORMAT_STRING;
    arguments[1].u.string = const_cast<char*>(locator.c_str());
    arguments[2].format = MPV_FORMAT_STRING;
    arguments[2].u.string = replace_mode.data();
    // The Switch libmpv build in this project follows the older loadfile
    // command shape: loadfile <url> [<flags> [<options>]].
    // Passing the newer optional index argument makes the command fail with
    // "invalid parameter", so keep the options map as the 4th argument.
    arguments[3].format = MPV_FORMAT_NODE_MAP;
    arguments[3].u.list = &option_map;

    mpv_node_list command_array {};
    command_array.num = 4;
    command_array.values = arguments.data();

    mpv_node root {};
    root.format = MPV_FORMAT_NODE_ARRAY;
    root.u.list = &command_array;
    return mpv_command_node_async(
        handle,
        static_cast<uint64_t>(AsyncCommandKind::LoadFile),
        &root);
}

constexpr const char* kHwdecCodecs =
    "mpeg1video,mpeg2video,mpeg4,vc1,wmv3,h264,hevc,vp8,vp9,mjpeg";
constexpr int kMpvImageCount = 3;
constexpr unsigned kPresentCmdBufSlices = 3;
constexpr uint32_t kPresentCmdBufSliceSize = 0x10000;

struct ParsedTrackInfo {
    int id = -1;
    std::string type;
    std::string lang;
    std::string title;
    std::string label;
    bool selected = false;
    bool default_track = false;
};

enum class PreferredLanguageMode {
    None,
    Chinese,
    English,
    Custom,
};

std::string ascii_lower(std::string value) {
    for (char& character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return value;
}

std::string normalized_token(std::string value) {
    return ascii_lower(trim(std::move(value)));
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool should_capture_iptv_warning_as_error(
    PlaybackSourceKind active_source_kind,
    const std::string& prefix,
    const std::string& level,
    const std::string& text) {
    if (active_source_kind != PlaybackSourceKind::Iptv) {
        return false;
    }

    const std::string normalized_text = ascii_lower(text);
    if (prefix == "stream" && level == "error") {
        return true;
    }

    if (prefix != "ffmpeg") {
        return false;
    }

    if (level == "fatal" || level == "error") {
        return true;
    }

    if (level != "warn") {
        return false;
    }

    return normalized_text.find("http error") != std::string::npos ||
           normalized_text.find("server returned") != std::string::npos ||
           normalized_text.find("failed to resolve hostname") != std::string::npos ||
           normalized_text.find("connection refused") != std::string::npos ||
           normalized_text.find("forbidden") != std::string::npos;
}

bool looks_like_domain_segment(const std::string& value) {
    if (value.empty() || value.find('.') == std::string::npos) {
        return false;
    }

    if (value.front() == '.' || value.back() == '.' || value.front() == '-' || value.back() == '-') {
        return false;
    }

    for (const unsigned char character : value) {
        if (!(std::isalnum(character) != 0 || character == '.' || character == '-')) {
            return false;
        }
    }

    return true;
}

std::string maybe_rewrite_ipv6_literal_http_locator(const std::string& locator) {
    const std::string lower = ascii_lower(locator);
    if (!starts_with(lower, "http://") && !starts_with(lower, "https://")) {
        return locator;
    }

    const size_t scheme_separator = locator.find("://");
    if (scheme_separator == std::string::npos) {
        return locator;
    }

    const size_t host_begin = scheme_separator + 3;
    if (host_begin >= locator.size() || locator[host_begin] != '[') {
        return locator;
    }

    const size_t host_end = locator.find(']', host_begin + 1);
    if (host_end == std::string::npos) {
        return locator;
    }

    size_t path_begin = std::string::npos;
    std::string port_text;
    const size_t after_host = host_end + 1;
    if (after_host < locator.size() && locator[after_host] == ':') {
        const size_t port_begin = after_host + 1;
        const size_t port_end = locator.find_first_of("/?#", port_begin);
        if (port_end == std::string::npos) {
            return locator;
        }

        port_text = locator.substr(port_begin, port_end - port_begin);
        if (port_text.empty() ||
            !std::all_of(port_text.begin(), port_text.end(), [](unsigned char character) {
                return std::isdigit(character) != 0;
            })) {
            return locator;
        }

        path_begin = port_end;
    } else {
        path_begin = locator.find_first_of("/?#", after_host);
        if (path_begin == std::string::npos) {
            return locator;
        }
    }

    if (path_begin >= locator.size() || locator[path_begin] != '/') {
        return locator;
    }

    const size_t suffix_begin = locator.find_first_of("?#", path_begin);
    const std::string path =
        suffix_begin == std::string::npos ? locator.substr(path_begin)
                                          : locator.substr(path_begin, suffix_begin - path_begin);
    const std::string suffix = suffix_begin == std::string::npos ? std::string {} : locator.substr(suffix_begin);

    const size_t first_segment_end = path.find('/', 1);
    const std::string first_segment =
        first_segment_end == std::string::npos ? path.substr(1) : path.substr(1, first_segment_end - 1);
    if (!looks_like_domain_segment(first_segment)) {
        return locator;
    }

    const std::string remaining_path =
        first_segment_end == std::string::npos ? std::string("/") : path.substr(first_segment_end);

    std::string rewritten = locator.substr(0, host_begin);
    rewritten += first_segment;
    if (!port_text.empty()) {
        rewritten += ":";
        rewritten += port_text;
    }
    rewritten += remaining_path;
    rewritten += suffix;
    return rewritten;
}

std::string playback_source_kind_label(PlaybackSourceKind kind) {
    switch (kind) {
        case PlaybackSourceKind::Smb:
            return "smb";
        case PlaybackSourceKind::Iptv:
            return "iptv";
        default:
            return "unknown";
    }
}

std::string detect_locator_stream_kind(const std::string& locator) {
    std::string lower = ascii_lower(locator);
    if (lower.find("youtube.com") != std::string::npos || lower.find("youtu.be") != std::string::npos) {
        return "YouTube";
    }
    if (lower.find("twitch.tv") != std::string::npos) {
        return "Twitch";
    }
    if (starts_with(lower, "edl://")) {
        return "HLS";
    }
    if (starts_with(lower, "hex://") || starts_with(lower, "memory://")) {
        return "HLS";
    }
    if (starts_with(lower, "data://")) {
        return "HLS";
    }
    if (lower.find(".m3u8") != std::string::npos) {
        return "HLS";
    }
    if (lower.find(".mpd") != std::string::npos) {
        return "DASH";
    }
    if (starts_with(lower, "rtmp://")) {
        return "RTMP";
    }
    if (starts_with(lower, "http://") || starts_with(lower, "https://")) {
        return "HTTP";
    }
    return {};
}

bool language_code_matches(const std::string& actual, const std::string& expected) {
    if (actual.empty() || expected.empty()) {
        return false;
    }

    if (actual == expected) {
        return true;
    }

    return starts_with(actual, expected + "-") || starts_with(actual, expected + "_");
}

bool contains_keyword(const std::string& haystack, const std::string& needle) {
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

std::string build_track_search_text(const ParsedTrackInfo& track) {
    return ascii_lower(track.lang + " " + track.title + " " + track.label);
}

std::vector<std::string> split_preference_tokens(const std::string& value) {
    std::vector<std::string> tokens;
    std::string current;

    const auto flush = [&tokens, &current]() {
        std::string token = normalized_token(current);
        current.clear();
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
    };

    for (char character : value) {
        if (character == ',' || character == ';' || character == '|' || std::isspace(static_cast<unsigned char>(character))) {
            flush();
            continue;
        }

        current.push_back(character);
    }

    flush();
    return tokens;
}

PreferredLanguageMode detect_preferred_language_mode(const std::string& value) {
    const std::string normalized = normalized_token(value);
    if (normalized.empty()) {
        return PreferredLanguageMode::None;
    }
    if (normalized == "zh") {
        return PreferredLanguageMode::Chinese;
    }
    if (normalized == "en") {
        return PreferredLanguageMode::English;
    }
    return PreferredLanguageMode::Custom;
}

std::string build_audio_language_hint(const switchbox::core::GeneralSettings& general) {
    if (!general.use_preferred_audio_language) {
        return {};
    }

    const std::string normalized = normalized_token(general.preferred_audio_language);
    if (normalized == "zh") {
        return "cmn,zh-cn,zh,chi,zho,yue";
    }
    if (normalized == "en") {
        return "eng,en";
    }

    return trim(general.preferred_audio_language);
}

std::string build_subtitle_language_hint(const switchbox::core::GeneralSettings& general) {
    if (!general.use_preferred_subtitle_language) {
        return {};
    }

    const std::string normalized = normalized_token(general.preferred_subtitle_language);
    if (normalized == "zh") {
        return "zh-hans,zh-cn,chs,sc,zh,chi,zho,zh-hant,zh-tw,cht,tc";
    }
    if (normalized == "en") {
        return "eng,en";
    }

    return trim(general.preferred_subtitle_language);
}

int score_audio_track_for_preference(const ParsedTrackInfo& track, const std::string& preference) {
    const std::string language = normalized_token(track.lang);
    const std::string text = build_track_search_text(track);

    switch (detect_preferred_language_mode(preference)) {
        case PreferredLanguageMode::Chinese:
            if (language_code_matches(language, "cmn") ||
                language_code_matches(language, "zh-cn") ||
                contains_keyword(text, "mandarin") ||
                contains_keyword(text, "putonghua") ||
                contains_keyword(text, "普通话") ||
                contains_keyword(text, "国语")) {
                return 4000;
            }
            if (language_code_matches(language, "yue") ||
                contains_keyword(text, "cantonese") ||
                contains_keyword(text, "粤语")) {
                return 2000;
            }
            if (language_code_matches(language, "zh") ||
                language_code_matches(language, "chi") ||
                language_code_matches(language, "zho") ||
                contains_keyword(text, "chinese") ||
                contains_keyword(text, "中文") ||
                contains_keyword(text, "汉语") ||
                contains_keyword(text, "華語") ||
                contains_keyword(text, "华语")) {
                return 3000;
            }
            return 0;
        case PreferredLanguageMode::English:
            if (language_code_matches(language, "eng") ||
                language_code_matches(language, "en") ||
                contains_keyword(text, "english") ||
                contains_keyword(text, "英语") ||
                contains_keyword(text, "英文")) {
                return 3000;
            }
            return 0;
        case PreferredLanguageMode::Custom: {
            int best_score = 0;
            for (const auto& token : split_preference_tokens(preference)) {
                if (language_code_matches(language, token)) {
                    best_score = std::max(best_score, 3000);
                    continue;
                }
                if (contains_keyword(text, token)) {
                    best_score = std::max(best_score, 2000);
                }
            }
            return best_score;
        }
        case PreferredLanguageMode::None:
        default:
            return 0;
    }
}

int score_subtitle_track_for_preference(const ParsedTrackInfo& track, const std::string& preference) {
    const std::string language = normalized_token(track.lang);
    const std::string text = build_track_search_text(track);

    switch (detect_preferred_language_mode(preference)) {
        case PreferredLanguageMode::Chinese:
            if (language_code_matches(language, "zh-hans") ||
                language_code_matches(language, "zh-cn") ||
                language_code_matches(language, "chs") ||
                language_code_matches(language, "sc") ||
                contains_keyword(text, "simplified") ||
                contains_keyword(text, "简体") ||
                contains_keyword(text, "简中")) {
                return 4000;
            }
            if (language_code_matches(language, "zh-hant") ||
                language_code_matches(language, "zh-tw") ||
                language_code_matches(language, "cht") ||
                language_code_matches(language, "tc") ||
                contains_keyword(text, "traditional") ||
                contains_keyword(text, "繁体") ||
                contains_keyword(text, "繁中")) {
                return 2000;
            }
            if (language_code_matches(language, "zh") ||
                language_code_matches(language, "chi") ||
                language_code_matches(language, "zho") ||
                contains_keyword(text, "chinese") ||
                contains_keyword(text, "中文")) {
                return 3000;
            }
            return 0;
        case PreferredLanguageMode::English:
            if (language_code_matches(language, "eng") ||
                language_code_matches(language, "en") ||
                contains_keyword(text, "english") ||
                contains_keyword(text, "英语") ||
                contains_keyword(text, "英文")) {
                return 3000;
            }
            return 0;
        case PreferredLanguageMode::Custom: {
            int best_score = 0;
            for (const auto& token : split_preference_tokens(preference)) {
                if (language_code_matches(language, token)) {
                    best_score = std::max(best_score, 3000);
                    continue;
                }
                if (contains_keyword(text, token)) {
                    best_score = std::max(best_score, 2000);
                }
            }
            return best_score;
        }
        case PreferredLanguageMode::None:
        default:
            return 0;
    }
}

int count_tracks_by_type(const std::vector<ParsedTrackInfo>& tracks, const char* type) {
    int count = 0;
    for (const auto& track : tracks) {
        if (track.type == type) {
            ++count;
        }
    }
    return count;
}

int find_default_track_id(const std::vector<ParsedTrackInfo>& tracks, const char* type) {
    for (const auto& track : tracks) {
        if (track.type == type && track.default_track) {
            return track.id;
        }
    }

    return -1;
}

int find_first_track_id(const std::vector<ParsedTrackInfo>& tracks, const char* type) {
    for (const auto& track : tracks) {
        if (track.type == type) {
            return track.id;
        }
    }

    return -1;
}

uint32_t align_up_u32(uint64_t value, uint32_t alignment) {
    const uint64_t aligned = (value + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1);
    return static_cast<uint32_t>(aligned);
}

uint64_t monotonic_milliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

class SwitchMpvPlayer {
public:
    static SwitchMpvPlayer& instance() {
        static SwitchMpvPlayer player;
        return player;
    }

    bool prepare_renderer_for_switch(
        DkDevice device,
        DkQueue queue,
        int width,
        int height,
        std::string& error_message) {
        if (!initialize(error_message)) {
            return false;
        }

        if (!ensure_deko3d_context(device, queue, width, height, error_message)) {
            if (!error_message.empty()) {
                this->last_error = error_message;
            }
            return false;
        }

        return true;
    }

    bool open(const PlaybackTarget& target, std::string& error_message) {
        std::string locator;
        if (this->needs_full_reset_before_next_open) {
            append_debug_log("[open] pending deferred-stop cleanup before open");
            this->suppress_stop_related_errors = true;
            process_pending_events(0.0);
            if (!is_idle_active() && this->handle != nullptr) {
                const char* command[] = {"stop", nullptr};
                const int rc = mpv_command_async(
                    this->handle,
                    static_cast<uint64_t>(AsyncCommandKind::Stop),
                    command);
                if (rc < 0) {
                    append_debug_log(
                        std::string("[open] deferred-stop cleanup stop command failed: ") + mpv_error_string(rc));
                } else {
                    append_debug_log("[open] deferred-stop cleanup stop command queued");
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                    do {
                        process_pending_events(0.02);
                        if (is_idle_active()) {
                            break;
                        }
                    } while (std::chrono::steady_clock::now() < deadline);
                }
            }
            process_pending_events(0.0);
            this->suppress_stop_related_errors = false;
            this->session_active.store(false);
            this->has_media = false;
            this->paused.store(false);
            this->frame_dirty.store(false);
            this->current_render_slot.store(-1);
            this->preferred_track_selection_pending = false;
            this->preferred_track_selection_applied = false;
            this->preferred_track_selection_waiting_for_restart = false;
            this->preferred_track_selection_attempts = 0;
            this->force_audio_track_reapply_for_current_file = false;
            this->rendered_frame_for_current_file.store(false);
            this->playback_started_at_ms.store(0);
            this->playback_restart_at_ms.store(0);
            this->last_render_kick_at_ms.store(0);
            this->last_audio_reapply_at_ms.store(0);
            this->open_requested_at_ms.store(0);
            this->active_source_kind = PlaybackSourceKind::Unknown;
            this->needs_full_reset_before_next_open = false;
            append_debug_log(
                "[open] deferred-stop cleanup complete idle=" + std::string(is_idle_active() ? "true" : "false"));
        } else if (this->session_active.load() || this->has_media || !is_idle_active()) {
            append_debug_log("[open] existing session detected, resetting backend before open");
            reset_backend_core_state();
            switchbox::core::switch_smb_mount_release();
        }

        if (!initialize(error_message)) {
            return false;
        }

        if (target.source_kind == PlaybackSourceKind::Smb) {
            std::optional<PlaybackTarget::SmbLocator> smb_locator = target.smb_locator;
            if (!smb_locator.has_value()) {
                PlaybackTarget::SmbLocator parsed_locator;
                if (try_parse_smb_locator_from_uri(pick_locator(target), parsed_locator)) {
                    append_debug_log("[open] smb_locator reconstructed from uri");
                    smb_locator = std::move(parsed_locator);
                }
            }

            if (smb_locator.has_value() &&
                !switchbox::core::switch_smb_mount_resolve_playback_path(
                    smb_locator.value(),
                    locator,
                    error_message)) {
                this->last_error = error_message;
                append_debug_log("[open] SMB mount failed: " + error_message);
                return false;
            }
            if (!locator.empty()) {
                append_debug_log("[open] smb_mounted locator=" + summarize_locator_for_debug_log(locator));
            } else {
                switchbox::core::switch_smb_mount_release();
                locator = pick_locator(target);
                append_debug_log("[open] smb_mount unavailable, fallback locator=" + summarize_locator_for_debug_log(locator));
            }
        } else {
            switchbox::core::switch_smb_mount_release();
            locator = pick_locator(target);
            if (target.source_kind == PlaybackSourceKind::Iptv) {
                if (!target.locator_pre_resolved) {
                    const std::string resolved_locator =
                        switchbox::core::resolve_iptv_stream_url_for_playback(locator);
                    if (!resolved_locator.empty() && resolved_locator != locator) {
                        append_debug_log("[open] resolved_hls_variant from=" + locator + " to=" + resolved_locator);
                        locator = resolved_locator;
                    }
                }
                const std::string rewritten_locator = maybe_rewrite_ipv6_literal_http_locator(locator);
                if (rewritten_locator != locator) {
                    append_debug_log("[open] ipv6_host_rewrite from=" + locator + " to=" + rewritten_locator);
                    locator = rewritten_locator;
                }
            }
        }

        if (locator.empty()) {
            error_message = "The selected target does not expose an openable locator yet.";
            return false;
        }

        this->last_error.clear();
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(true);
        this->playback_speed = 1.0;
        this->video_rotation_degrees = 0;
        this->playback_volume = std::clamp(switchbox::core::AppConfigStore::current().general.player_volume, 0, 100);
        this->current_render_slot.store(-1);
        this->active_iptv_stream_class = switchbox::core::IptvPreparedStreamClass::Unknown;
        this->disable_auto_track_selection_for_current_file = false;
        this->preferred_track_selection_pending = false;
        this->preferred_track_selection_applied = false;
        this->preferred_track_selection_waiting_for_restart = false;
        this->preferred_track_selection_attempts = 0;
        this->force_audio_track_reapply_for_current_file = false;
        this->suppress_stop_related_errors = false;
        this->rendered_frame_for_current_file.store(false);
        this->playback_started_at_ms.store(0);
        this->playback_restart_at_ms.store(0);
        this->last_render_kick_at_ms.store(0);
        this->last_audio_reapply_at_ms.store(0);
        this->open_requested_at_ms.store(monotonic_milliseconds());
        this->active_source_kind = target.source_kind;
        if (target.source_kind == PlaybackSourceKind::Iptv && target.iptv_open_plan.has_value()) {
            this->active_iptv_stream_class = target.iptv_open_plan->stream_class;
            this->disable_auto_track_selection_for_current_file =
                target.iptv_open_plan->stream_class == switchbox::core::IptvPreparedStreamClass::DirectFlv;
        }

        const auto open_options = build_iptv_open_options(target, locator);
        append_debug_log(
            "[open] source_kind=" + playback_source_kind_label(target.source_kind) +
            " stream_kind=" + detect_locator_stream_kind(locator) +
            " title=" + target.title +
            " locator=" + summarize_locator_for_debug_log(locator));
        if (target.source_kind == PlaybackSourceKind::Iptv && !open_options.empty()) {
            append_debug_log(
                "[open] iptv_options count=" + std::to_string(open_options.size()) +
                " has_user_agent=" + std::string(trim(target.http_user_agent).empty() ? "false" : "true") +
                " has_referrer=" + std::string(trim(target.http_referrer).empty() ? "false" : "true") +
                " header_count=" + std::to_string(target.http_header_fields.size()));
            append_debug_log("[open] iptv_option_keys=" + join_option_keys_for_debug_log(open_options));
            if (target.iptv_open_plan.has_value() && !target.iptv_open_plan->debug_summary.empty()) {
                append_debug_log("[open] iptv_plan " + target.iptv_open_plan->debug_summary);
            }
        }

        this->session_active.store(true);
        apply_runtime_preferences();
        apply_source_open_tuning(target);
        (void)apply_video_rotation_degrees(0, false);

        const int rc = command_loadfile_with_options(this->handle, locator, open_options);
        if (rc < 0) {
            this->session_active.store(false);
            error_message = mpv_error_string(rc);
            append_debug_log(std::string("[open] mpv_command_async failed: ") + error_message);
            return false;
        }

        append_debug_log("[open] mpv_command_async queued successfully");
        this->set_speed(1.0);
        this->set_volume(this->playback_volume);
        process_pending_events();
        return true;
    }

    void stop() {
        append_debug_log("[stop] requested");
        if (this->handle != nullptr) {
            const char* command[] = {"stop", nullptr};
            const int rc = mpv_command_async(
                this->handle,
                static_cast<uint64_t>(AsyncCommandKind::Stop),
                command);
            if (rc < 0) {
                append_debug_log(std::string("[stop] async stop command failed: ") + mpv_error_string(rc));
            } else {
                append_debug_log("[stop] async stop command queued");
            }
        }

        this->session_active.store(false);
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(false);
        this->current_render_slot.store(-1);
        this->preferred_track_selection_pending = false;
        this->preferred_track_selection_applied = false;
        this->preferred_track_selection_waiting_for_restart = false;
        this->preferred_track_selection_attempts = 0;
        this->force_audio_track_reapply_for_current_file = false;
        this->rendered_frame_for_current_file.store(false);
        this->playback_started_at_ms.store(0);
        this->playback_restart_at_ms.store(0);
        this->last_render_kick_at_ms.store(0);
        this->last_audio_reapply_at_ms.store(0);
        this->open_requested_at_ms.store(0);
        this->active_iptv_stream_class = switchbox::core::IptvPreparedStreamClass::Unknown;
        this->disable_auto_track_selection_for_current_file = false;
        this->needs_full_reset_before_next_open = true;
        this->active_source_kind = PlaybackSourceKind::Unknown;
        append_debug_log("[stop] deferred backend reset until next open");
    }

    void shutdown() {
        reset_backend_core_state();
        destroy_render_buffers();
        this->deko3d_device = nullptr;
        this->deko3d_queue = nullptr;
        this->last_error.clear();
        this->has_loaded_media_before = false;
    }

    bool is_session_active() const {
        return this->session_active.load();
    }

    bool has_active_media() const {
        return this->has_media;
    }

    bool has_rendered_frame_for_current_file() const {
        return this->rendered_frame_for_current_file.load();
    }

    void toggle_pause() {
        if (this->handle == nullptr) {
            return;
        }

        if (!this->session_active.load() && !this->has_media) {
            append_debug_log("[input] toggle_pause ignored: no active session");
            return;
        }

        int paused_flag = this->paused.load() ? 1 : 0;
        const int get_result = mpv_get_property(this->handle, "pause", MPV_FORMAT_FLAG, &paused_flag);
        if (get_result < 0) {
            append_debug_log(std::string("[input] mpv_get_property(pause) failed: ") + mpv_error_string(get_result));
            paused_flag = this->paused.load() ? 1 : 0;
        }

        int next_pause_flag = paused_flag == 0 ? 1 : 0;
        const int set_result = mpv_set_property(this->handle, "pause", MPV_FORMAT_FLAG, &next_pause_flag);
        if (set_result < 0) {
            append_debug_log(std::string("[input] mpv_set_property(pause) failed: ") + mpv_error_string(set_result));
            return;
        }

        this->paused.store(next_pause_flag != 0);
        append_debug_log(std::string("[input] toggle_pause applied, pause=") + (next_pause_flag != 0 ? "true" : "false"));
        process_pending_events();
    }

    bool seek_relative_seconds(double delta_seconds) {
        if (this->handle == nullptr) {
            return false;
        }

        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(3);
        stream << delta_seconds;
        const std::string delta_text = stream.str();
        const char* command[] = {"seek", delta_text.c_str(), "relative", "keyframes", nullptr};
        const int rc = mpv_command_async(
            this->handle,
            static_cast<uint64_t>(AsyncCommandKind::Seek),
            command);
        if (rc < 0) {
            append_debug_log(
                std::string("[input] seek ignored after async queue failure: ") + mpv_error_string(rc));
            return false;
        }

        return true;
    }

    bool seek_absolute_seconds(double target_seconds) {
        if (this->handle == nullptr) {
            return false;
        }

        if (target_seconds < 0.0) {
            target_seconds = 0.0;
        }

        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(3);
        stream << target_seconds;
        const std::string target_text = stream.str();
        const char* command[] = {"seek", target_text.c_str(), "absolute", "exact", nullptr};
        const int rc = mpv_command_async(
            this->handle,
            static_cast<uint64_t>(AsyncCommandKind::Seek),
            command);
        if (rc < 0) {
            append_debug_log(
                std::string("[input] absolute seek ignored after async queue failure: ") + mpv_error_string(rc));
            return false;
        }

        return true;
    }

    bool set_speed(double speed) {
        if (this->handle == nullptr) {
            return false;
        }

        if (speed < 0.1) {
            speed = 0.1;
        }
        if (speed > 8.0) {
            speed = 8.0;
        }

        double value = speed;
        const int rc = mpv_set_property(this->handle, "speed", MPV_FORMAT_DOUBLE, &value);
        if (rc < 0) {
            this->last_error = std::string("Set speed failed: ") + mpv_error_string(rc);
            append_debug_log("[input] set_speed failed: " + this->last_error);
            return false;
        }

        this->playback_speed = value;
        return true;
    }

    double get_speed() const {
        return this->playback_speed;
    }

    bool rotate_clockwise_90() {
        return apply_video_rotation_degrees((this->video_rotation_degrees + 90) % 360, true);
    }

    int get_video_rotation_degrees() const {
        return this->video_rotation_degrees;
    }

    bool set_volume(int volume) {
        if (this->handle == nullptr) {
            return false;
        }

        volume = std::clamp(volume, 0, 100);
        double value = static_cast<double>(volume);
        const int rc = mpv_set_property(this->handle, "volume", MPV_FORMAT_DOUBLE, &value);
        if (rc < 0) {
            this->last_error = std::string("Set volume failed: ") + mpv_error_string(rc);
            append_debug_log("[input] set_volume failed: " + this->last_error);
            return false;
        }

        this->playback_volume = volume;
        return true;
    }

    int get_volume() const {
        return this->playback_volume;
    }

    int64_t get_transfer_speed_bytes_per_second() {
        if (this->handle == nullptr) {
            return 0;
        }

        int64_t value = 0;
        const int rc = mpv_get_property(this->handle, "cache-speed", MPV_FORMAT_INT64, &value);
        if (rc < 0 || value < 0) {
            return 0;
        }

        return value;
    }

    uint64_t get_playback_restart_at_ms() {
        process_pending_events();
        return this->playback_restart_at_ms.load();
    }

    bool read_track_list(std::vector<ParsedTrackInfo>& tracks) {
        tracks.clear();
        if (this->handle == nullptr) {
            return false;
        }

        mpv_node root {};
        const int rc = mpv_get_property(this->handle, "track-list", MPV_FORMAT_NODE, &root);
        if (rc < 0) {
            return false;
        }

        if (root.format == MPV_FORMAT_NODE_ARRAY && root.u.list != nullptr) {
            const mpv_node_list* array = root.u.list;
            tracks.reserve(static_cast<size_t>(array->num));
            for (int index = 0; index < array->num; ++index) {
                const mpv_node& item = array->values[index];
                if (item.format != MPV_FORMAT_NODE_MAP || item.u.list == nullptr) {
                    continue;
                }

                const mpv_node_list* map = item.u.list;
                int id = -1;
                if (!node_to_int(node_map_find(map, "id"), id)) {
                    continue;
                }

                tracks.push_back({
                    .id = id,
                    .type = node_to_string(node_map_find(map, "type")),
                    .lang = trim(node_to_string(node_map_find(map, "lang"))),
                    .title = trim(node_to_string(node_map_find(map, "title"))),
                    .label = build_track_label(map, id),
                    .selected = node_to_bool(node_map_find(map, "selected")),
                    .default_track = node_to_bool(node_map_find(map, "default")),
                });
            }
        }

        mpv_free_node_contents(&root);
        return true;
    }

    std::vector<MpvTrackOption> list_tracks_by_type(const char* track_type) {
        std::vector<MpvTrackOption> options;
        if (this->handle == nullptr || track_type == nullptr) {
            return options;
        }

        std::vector<ParsedTrackInfo> tracks;
        if (!read_track_list(tracks)) {
            return options;
        }

        for (const auto& track : tracks) {
            if (track.type != track_type) {
                continue;
            }

            options.push_back({
                .id = track.id,
                .label = track.label,
                .selected = track.selected,
            });
        }
        return options;
    }

    std::vector<MpvTrackOption> list_audio_tracks() {
        return list_tracks_by_type("audio");
    }

    std::vector<MpvTrackOption> list_subtitle_tracks() {
        return list_tracks_by_type("sub");
    }

    bool set_audio_track_internal(int id, std::string* error_message = nullptr) {
        if (this->handle == nullptr) {
            if (error_message != nullptr) {
                *error_message = "Player backend is not ready.";
            }
            return false;
        }

        int64_t value = static_cast<int64_t>(id);
        const int rc = mpv_set_property(this->handle, "aid", MPV_FORMAT_INT64, &value);
        if (rc < 0) {
            if (error_message != nullptr) {
                *error_message = mpv_error_string(rc);
            }
            return false;
        }

        return true;
    }

    bool set_subtitle_track_internal(int id, std::string* error_message = nullptr) {
        if (this->handle == nullptr) {
            if (error_message != nullptr) {
                *error_message = "Player backend is not ready.";
            }
            return false;
        }

        int rc = 0;
        if (id < 0) {
            rc = mpv_set_property_string(this->handle, "sid", "no");
        } else {
            int64_t value = static_cast<int64_t>(id);
            rc = mpv_set_property(this->handle, "sid", MPV_FORMAT_INT64, &value);
        }

        if (rc < 0) {
            if (error_message != nullptr) {
                *error_message = mpv_error_string(rc);
            }
            return false;
        }

        return true;
    }

    int find_selected_track_id(const std::vector<ParsedTrackInfo>& tracks, const char* type) {
        for (const auto& track : tracks) {
            if (track.type == type && track.selected) {
                return track.id;
            }
        }

        return -1;
    }

    int choose_preferred_track_id(
        const std::vector<ParsedTrackInfo>& tracks,
        const char* type,
        const std::string& preference,
        bool subtitle) {
        int best_id = -1;
        int best_score = 0;

        for (const auto& track : tracks) {
            if (track.type != type) {
                continue;
            }

            const int score = subtitle ? score_subtitle_track_for_preference(track, preference)
                                       : score_audio_track_for_preference(track, preference);
            if (score > best_score || (score == best_score && score > 0 && track.selected)) {
                best_score = score;
                best_id = track.id;
            }
        }

        return best_id;
    }

    int choose_audio_track_id_for_activation(
        const std::vector<ParsedTrackInfo>& tracks,
        const switchbox::core::GeneralSettings& general) {
        if (general.use_preferred_audio_language) {
            const int preferred_audio_id =
                choose_preferred_track_id(tracks, "audio", general.preferred_audio_language, false);
            if (preferred_audio_id >= 0) {
                return preferred_audio_id;
            }
        }

        const int selected_audio_id = find_selected_track_id(tracks, "audio");
        if (selected_audio_id >= 0) {
            return selected_audio_id;
        }

        const int default_audio_id = find_default_track_id(tracks, "audio");
        if (default_audio_id >= 0) {
            return default_audio_id;
        }

        return find_first_track_id(tracks, "audio");
    }

    int choose_subtitle_track_id_for_activation(
        const std::vector<ParsedTrackInfo>& tracks,
        const switchbox::core::GeneralSettings& general) {
        if (!general.use_preferred_subtitle_language) {
            return -1;
        }

        const int preferred_subtitle_id =
            choose_preferred_track_id(tracks, "sub", general.preferred_subtitle_language, true);
        if (preferred_subtitle_id >= 0) {
            return preferred_subtitle_id;
        }

        const int selected_subtitle_id = find_selected_track_id(tracks, "sub");
        if (selected_subtitle_id >= 0) {
            return selected_subtitle_id;
        }

        const int default_subtitle_id = find_default_track_id(tracks, "sub");
        if (default_subtitle_id >= 0) {
            return default_subtitle_id;
        }

        if (count_tracks_by_type(tracks, "sub") == 1) {
            return find_first_track_id(tracks, "sub");
        }

        return -1;
    }

    bool try_apply_preferred_track_languages_once() {
        if (!this->preferred_track_selection_pending || this->preferred_track_selection_applied ||
            this->preferred_track_selection_waiting_for_restart) {
            return true;
        }

        std::vector<ParsedTrackInfo> tracks;
        if (!read_track_list(tracks)) {
            return false;
        }

        const auto& general = switchbox::core::AppConfigStore::current().general;
        const int attempt_index = ++this->preferred_track_selection_attempts;
        bool any_change = false;
        bool audio_done = true;
        bool subtitle_done = true;

        const int audio_track_count = count_tracks_by_type(tracks, "audio");
        const int subtitle_track_count = count_tracks_by_type(tracks, "sub");

        if (general.use_preferred_audio_language || this->force_audio_track_reapply_for_current_file) {
            const int selected_audio_id = find_selected_track_id(tracks, "audio");
            const int target_audio_id = choose_audio_track_id_for_activation(tracks, general);

            if (target_audio_id >= 0) {
                if (this->force_audio_track_reapply_for_current_file || target_audio_id != selected_audio_id) {
                    std::string error_message;
                    if (set_audio_track_internal(target_audio_id, &error_message)) {
                        any_change = true;
                        this->force_audio_track_reapply_for_current_file = false;
                        append_debug_log(
                            "[pref] activated audio track id=" + std::to_string(target_audio_id));
                    } else {
                        audio_done = false;
                        if (!error_message.empty()) {
                            append_debug_log("[pref] audio activation failed: " + error_message);
                        }
                    }
                } else {
                    this->force_audio_track_reapply_for_current_file = false;
                }
            } else if (audio_track_count > 0 && attempt_index < 12) {
                audio_done = false;
            } else {
                this->force_audio_track_reapply_for_current_file = false;
            }
        }

        if (general.use_preferred_subtitle_language) {
            const int selected_subtitle_id = find_selected_track_id(tracks, "sub");
            const int target_subtitle_id = choose_subtitle_track_id_for_activation(tracks, general);

            if (target_subtitle_id >= 0) {
                if (target_subtitle_id != selected_subtitle_id) {
                    std::string error_message;
                    if (set_subtitle_track_internal(target_subtitle_id, &error_message)) {
                        any_change = true;
                        append_debug_log(
                            "[pref] activated subtitle track id=" +
                            std::to_string(target_subtitle_id));
                    } else {
                        subtitle_done = false;
                        if (!error_message.empty()) {
                            append_debug_log("[pref] subtitle activation failed: " + error_message);
                        }
                    }
                }
            } else if (subtitle_track_count > 0 && attempt_index < 12) {
                subtitle_done = false;
            }
        }

        if (!audio_done || !subtitle_done) {
            return false;
        }

        this->preferred_track_selection_pending = false;
        this->preferred_track_selection_applied = true;

        if (any_change) {
            process_pending_events();
            request_render_update();
        }

        return true;
    }

    void maybe_request_render_kick() {
        if (!(this->session_active.load() || this->has_media) || this->context == nullptr) {
            return;
        }

        const int slot = this->current_render_slot.load();
        if (slot >= 0 && !this->frame_dirty.load()) {
            return;
        }

        const uint64_t now_ms = monotonic_milliseconds();
        const uint64_t last_kick_ms = this->last_render_kick_at_ms.load();
        if (last_kick_ms != 0 && now_ms - last_kick_ms < 100) {
            return;
        }

        this->last_render_kick_at_ms.store(now_ms);
        request_render_update();
    }

    void maybe_force_audio_track_reapply_after_black_screen() {
        if (this->disable_auto_track_selection_for_current_file ||
            !this->force_audio_track_reapply_for_current_file ||
            this->rendered_frame_for_current_file.load()) {
            return;
        }

        const uint64_t restart_at_ms = this->playback_restart_at_ms.load();
        if (restart_at_ms == 0) {
            return;
        }

        const uint64_t now_ms = monotonic_milliseconds();
        if (now_ms <= restart_at_ms || now_ms - restart_at_ms < 300) {
            return;
        }

        const uint64_t last_reapply_ms = this->last_audio_reapply_at_ms.load();
        if (last_reapply_ms != 0 && now_ms - last_reapply_ms < 500) {
            return;
        }

        std::vector<ParsedTrackInfo> tracks;
        if (!read_track_list(tracks)) {
            return;
        }

        const int target_audio_id =
            choose_audio_track_id_for_activation(tracks, switchbox::core::AppConfigStore::current().general);
        if (target_audio_id < 0) {
            return;
        }

        std::string error_message;
        if (!set_audio_track_internal(target_audio_id, &error_message)) {
            if (!error_message.empty()) {
                append_debug_log("[recover] delayed audio reapply failed: " + error_message);
            }
            this->last_audio_reapply_at_ms.store(now_ms);
            return;
        }

        this->last_audio_reapply_at_ms.store(now_ms);
        append_debug_log("[recover] delayed audio reapply id=" + std::to_string(target_audio_id));
        process_pending_events();
        request_render_update();
    }

    bool set_audio_track(int id, std::string& error_message) {
        if (!set_audio_track_internal(id, &error_message)) {
            return false;
        }

        process_pending_events();
        return true;
    }

    bool set_subtitle_track(int id, std::string& error_message) {
        if (!set_subtitle_track_internal(id, &error_message)) {
            return false;
        }

        process_pending_events();
        return true;
    }

    double get_position_seconds() {
        if (this->handle == nullptr) {
            return 0.0;
        }

        double position = 0.0;
        const int rc = mpv_get_property(this->handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
        if (rc < 0 || position < 0.0) {
            return 0.0;
        }

        return position;
    }

    double get_duration_seconds() {
        if (this->handle == nullptr) {
            return 0.0;
        }

        double duration = 0.0;
        const int rc = mpv_get_property(this->handle, "duration", MPV_FORMAT_DOUBLE, &duration);
        if (rc < 0 || duration <= 0.0) {
            return 0.0;
        }

        return duration;
    }

    bool is_paused() const {
        return this->paused.load();
    }

    bool render_deko3d_frame(
        DkDevice device,
        DkQueue queue,
        DkImage* texture,
        int width,
        int height) {
        process_pending_events();
        maybe_fail_slow_initial_load();
        maybe_request_render_kick();

        if (device == nullptr || queue == nullptr || texture == nullptr) {
            return false;
        }

        std::string error_message;
        if (!ensure_deko3d_context(device, queue, width, height, error_message)) {
            if (!error_message.empty()) {
                this->last_error = error_message;
            }
            return false;
        }

        if (!(this->session_active.load() || this->has_media)) {
            return false;
        }

        (void)try_apply_preferred_track_languages_once();
        maybe_force_audio_track_reapply_after_black_screen();

        const int slot = this->current_render_slot.load();
        if (slot < 0 || slot >= kMpvImageCount || !this->offscreen_ready) {
            return false;
        }

        this->present_cmd_fences[this->present_cmd_slice].wait();
        this->present_cmd_buf.clear();
        this->present_cmd_buf.addMemory(
            this->present_cmd_memblock,
            this->present_cmd_slice * kPresentCmdBufSliceSize,
            kPresentCmdBufSliceSize);
        auto& target_image = *reinterpret_cast<dk::Image*>(texture);

        this->present_cmd_buf.copyImage(
            dk::ImageView(this->mpv_images[slot]),
            DkImageRect {
                0,
                0,
                0,
                static_cast<uint32_t>(this->offscreen_width),
                static_cast<uint32_t>(this->offscreen_height),
                1,
            },
            dk::ImageView(target_image),
            DkImageRect {
                0,
                0,
                0,
                static_cast<uint32_t>(std::max(1, width)),
                static_cast<uint32_t>(std::max(1, height)),
                1,
            });
        this->present_cmd_buf.signalFence(this->present_cmd_fences[this->present_cmd_slice], false);
        dkQueueSubmitCommands(queue, this->present_cmd_buf.finishList());
        this->present_cmd_slice = (this->present_cmd_slice + 1) % kPresentCmdBufSlices;
        return true;
    }

    void report_render_swap() {
    }

    std::string consume_last_error() {
        std::string value = this->last_error;
        this->last_error.clear();
        return value;
    }

    std::string register_memory_text_stream(std::string extension, std::string data) {
        if (data.empty()) {
            return {};
        }

        extension = sanitize_memory_text_extension(std::move(extension));
        auto stream = std::make_shared<MemoryTextStreamData>();
        stream->bytes.assign(data.begin(), data.end());

        auto& registry = memory_text_stream_registry();
        std::string key;
        {
            std::scoped_lock lock(registry.mutex);
            const uint64_t id = ++registry.next_id;
            key = "stream-" + to_hex(id) + "-" + to_hex(fnv1a64(data)) + "." + extension;
            registry.streams[key] = stream;
        }

        const std::string locator = std::string(kMemoryTextProtocolPrefix) + key;
        append_debug_log(
            "[mem] registered locator=" + locator +
            " bytes=" + std::to_string(stream->bytes.size()));
        return locator;
    }

private:
    void reset_backend_core_state() {
        stop_render_thread();

        if (this->deko3d_queue != nullptr) {
            dkQueueWaitIdle(this->deko3d_queue);
        }

        if (this->context != nullptr) {
            mpv_render_context_free(this->context);
            this->context = nullptr;
        }

        if (this->handle != nullptr) {
            mpv_terminate_destroy(this->handle);
            this->handle = nullptr;
        }

        this->ready = false;
        this->memory_text_protocol_registered = false;
        this->has_media = false;
        this->session_active.store(false);
        this->paused.store(false);
        this->frame_dirty.store(false);
        this->current_render_slot.store(-1);
        this->playback_speed = 1.0;
        this->video_rotation_degrees = 0;
        this->mpv_redraw_count = 0;
        this->preferred_track_selection_pending = false;
        this->preferred_track_selection_applied = false;
        this->preferred_track_selection_waiting_for_restart = false;
        this->preferred_track_selection_attempts = 0;
        this->force_audio_track_reapply_for_current_file = false;
        this->rendered_frame_for_current_file.store(false);
        this->playback_started_at_ms.store(0);
        this->playback_restart_at_ms.store(0);
        this->last_render_kick_at_ms.store(0);
        this->last_audio_reapply_at_ms.store(0);
        this->open_requested_at_ms.store(0);
        this->needs_full_reset_before_next_open = false;
    }

    void request_render_update() {
        this->frame_dirty.store(true);
        {
            std::lock_guard<std::mutex> lock(this->mpv_redraw_mutex);
            ++this->mpv_redraw_count;
        }
        this->mpv_redraw_condvar.notify_one();
    }

    static void mpv_render_update(void* context) {
        auto* self = static_cast<SwitchMpvPlayer*>(context);
        if (self != nullptr) {
            self->request_render_update();
        }
    }

    bool is_idle_active() {
        if (this->handle == nullptr) {
            return true;
        }

        int idle_flag = 0;
        const int rc = mpv_get_property(this->handle, "idle-active", MPV_FORMAT_FLAG, &idle_flag);
        return rc >= 0 && idle_flag != 0;
    }

    bool wait_for_stop_completion(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            process_pending_events(0.05);

            if ((!this->session_active.load() && !this->has_media) || is_idle_active()) {
                this->session_active.store(false);
                this->has_media = false;
                this->paused.store(false);
                return true;
            }
        } while (std::chrono::steady_clock::now() < deadline);

        return (!this->session_active.load() && !this->has_media) || is_idle_active();
    }

    void maybe_fail_slow_initial_load() {
        if (!this->session_active.load() || this->has_media || this->handle == nullptr) {
            return;
        }

        const uint64_t requested_at_ms = this->open_requested_at_ms.load();
        if (requested_at_ms == 0) {
            return;
        }

        const uint64_t now_ms = monotonic_milliseconds();
        const uint64_t timeout_ms = initial_load_timeout_ms_for_source(this->active_source_kind);
        if (now_ms <= requested_at_ms || now_ms - requested_at_ms < timeout_ms) {
            return;
        }

        const uint64_t elapsed_ms = now_ms - requested_at_ms;
        this->open_requested_at_ms.store(0);
        this->session_active.store(false);
        this->has_media = false;
        this->paused.store(false);
        this->last_error = "Playback ended: loading timed out";
        append_debug_log(
            "[event] initial load timed out before FILE_LOADED source_kind=" +
            playback_source_kind_label(this->active_source_kind) +
            " waited_ms=" + std::to_string(elapsed_ms) +
            " timeout_ms=" + std::to_string(timeout_ms));

        const char* command[] = {"stop", nullptr};
        const int rc = mpv_command_async(
            this->handle,
            static_cast<uint64_t>(AsyncCommandKind::Stop),
            command);
        if (rc < 0) {
            append_debug_log(std::string("[event] timeout stop command failed: ") + mpv_error_string(rc));
        } else {
            append_debug_log("[event] timeout stop command queued");
        }
    }

    void set_property_string_relaxed(const char* property_name, const std::string& value) {
        if (this->handle == nullptr || property_name == nullptr) {
            return;
        }

        if (mpv_set_property_string(this->handle, property_name, value.c_str()) >= 0) {
            return;
        }

        const char* command[] = {"set", property_name, value.c_str(), nullptr};
        (void)mpv_command(this->handle, command);
    }

    void apply_source_open_tuning(const PlaybackTarget& target) {
        if (this->handle == nullptr) {
            return;
        }

        std::string analyzeduration = "0.4";
        std::string probescore = "24";
        if (target.source_kind == PlaybackSourceKind::Iptv && target.iptv_open_plan.has_value()) {
            if (!trim(target.iptv_open_plan->analyzeduration_override).empty()) {
                analyzeduration = trim(target.iptv_open_plan->analyzeduration_override);
            }
            if (!trim(target.iptv_open_plan->probescore_override).empty()) {
                probescore = trim(target.iptv_open_plan->probescore_override);
            }
        }

        set_property_string_relaxed("demuxer-lavf-analyzeduration", analyzeduration);
        set_property_string_relaxed("demuxer-lavf-probescore", probescore);
        append_debug_log(
            "[open] source_tuning analyzeduration=" + analyzeduration +
            " probescore=" + probescore +
            " source_kind=" + playback_source_kind_label(target.source_kind));
    }

    void apply_runtime_preferences() {
        if (this->handle == nullptr) {
            return;
        }

        const auto& general = switchbox::core::AppConfigStore::current().general;
        set_property_string_relaxed("hwdec", general.hardware_decode ? "nvtegra" : "no");
        if (general.hardware_decode) {
            set_property_string_relaxed("hwdec-codecs", kHwdecCodecs);
        }

        const int readahead_seconds = std::max(0, general.demux_cache_sec);
        set_property_string_relaxed("demuxer-seekable-cache", readahead_seconds > 0 ? "yes" : "no");
        set_property_string_relaxed("demuxer-readahead-secs", std::to_string(readahead_seconds));
        set_property_string_relaxed("alang", build_audio_language_hint(general));
        set_property_string_relaxed("slang", build_subtitle_language_hint(general));
    }

    bool apply_video_rotation_degrees(int degrees, bool update_error) {
        if (this->handle == nullptr) {
            return false;
        }

        degrees %= 360;
        if (degrees < 0) {
            degrees += 360;
        }

        int64_t value = static_cast<int64_t>(degrees);
        const int rc = mpv_set_property(this->handle, "video-rotate", MPV_FORMAT_INT64, &value);
        if (rc < 0) {
            if (update_error) {
                this->last_error = std::string("Set rotation failed: ") + mpv_error_string(rc);
                append_debug_log("[input] set_rotation failed: " + this->last_error);
            }
            return false;
        }

        this->video_rotation_degrees = degrees;
        return true;
    }

    void stop_render_thread() {
        if (!this->mpv_render_thread.joinable()) {
            return;
        }

        this->mpv_render_thread.request_stop();
        this->mpv_redraw_condvar.notify_all();
        this->mpv_render_thread.join();
    }

    void ensure_render_thread() {
        if (this->mpv_render_thread.joinable()) {
            return;
        }

        this->mpv_render_thread = std::jthread([this](std::stop_token stop_token) {
            dk::Fence done_fence {};
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(this->mpv_redraw_mutex);
                    this->mpv_redraw_condvar.wait(lock, [this, &stop_token] {
                        return stop_token.stop_requested() || this->mpv_redraw_count > 0;
                    });
                    if (stop_token.stop_requested()) {
                        break;
                    }
                    --this->mpv_redraw_count;
                }

                std::scoped_lock<std::mutex> render_lock(this->render_mutex);
                if (this->context == nullptr || !this->offscreen_ready) {
                    continue;
                }

                if ((mpv_render_context_update(this->context) & MPV_RENDER_UPDATE_FRAME) == 0) {
                    continue;
                }

                const int previous_slot = this->current_render_slot.load();
                const int next_slot = previous_slot >= 0 ? (previous_slot + 1) % kMpvImageCount : 0;

                mpv_deko3d_fbo fbo {
                    &this->mpv_images[next_slot],
                    &this->mpv_copy_fences[next_slot],
                    &done_fence,
                    this->offscreen_width,
                    this->offscreen_height,
                    DkImageFormat_RGBA8_Unorm,
                };
                mpv_render_param params[] = {
                    {MPV_RENDER_PARAM_DEKO3D_FBO, &fbo},
                    {MPV_RENDER_PARAM_INVALID, nullptr},
                };

                mpv_render_context_render(this->context, params);
                done_fence.wait();
                this->current_render_slot.store(next_slot);
                this->frame_dirty.store(false);
                this->rendered_frame_for_current_file.store(true);
                mpv_render_context_report_swap(this->context);
            }
        });
    }

    void destroy_render_buffers() {
        this->current_render_slot.store(-1);
        this->offscreen_ready = false;
        this->offscreen_width = 0;
        this->offscreen_height = 0;
        this->present_cmd_slice = 0;
        this->present_cmd_buf = nullptr;
        this->present_cmd_memblock = nullptr;
        this->mpv_images_memblock = nullptr;
    }

    bool ensure_render_buffers(DkDevice device, DkQueue queue, int width, int height, std::string& error_message) {
        const int safe_width = std::max(1, width);
        const int safe_height = std::max(1, height);
        if (this->offscreen_ready &&
            this->deko3d_device == device &&
            this->offscreen_width == safe_width &&
            this->offscreen_height == safe_height &&
            this->present_cmd_buf &&
            this->present_cmd_memblock &&
            this->mpv_images_memblock) {
            this->deko3d_queue = queue;
            return true;
        }

        std::scoped_lock<std::mutex> render_lock(this->render_mutex);
        this->deko3d_device = device;
        this->deko3d_queue = queue;
        if (this->deko3d_queue != nullptr) {
            dkQueueWaitIdle(this->deko3d_queue);
        }

        destroy_render_buffers();

        dk::ImageLayout layout;
        dk::ImageLayoutMaker { device }
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(static_cast<uint32_t>(safe_width), static_cast<uint32_t>(safe_height))
            .initialize(layout);

        const uint32_t image_block_alignment =
            std::max(layout.getAlignment(), static_cast<uint32_t>(DK_MEMBLOCK_ALIGNMENT));
        const uint32_t image_block_size = align_up_u32(layout.getSize(), image_block_alignment);
        this->mpv_images_memblock = dk::MemBlockMaker { device, image_block_size * kMpvImageCount }
                                       .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                                       .create();
        if (!this->mpv_images_memblock) {
            error_message = "Failed to allocate deko3d offscreen images.";
            return false;
        }

        for (int index = 0; index < kMpvImageCount; ++index) {
            this->mpv_images[index].initialize(layout, this->mpv_images_memblock, index * image_block_size);
        }

        const uint32_t cmd_mem_size = kPresentCmdBufSlices * kPresentCmdBufSliceSize;
        this->present_cmd_memblock = dk::MemBlockMaker { device, cmd_mem_size }
                                        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                                        .create();
        if (!this->present_cmd_memblock) {
            error_message = "Failed to allocate deko3d copy command memory.";
            destroy_render_buffers();
            return false;
        }

        this->present_cmd_buf = dk::CmdBufMaker { device }.create();
        if (!this->present_cmd_buf) {
            error_message = "Failed to create deko3d copy command buffer.";
            destroy_render_buffers();
            return false;
        }

        for (auto& fence : this->mpv_copy_fences) {
            dkQueueSignalFence(queue, &fence, false);
        }
        for (auto& fence : this->present_cmd_fences) {
            dkQueueSignalFence(queue, &fence, false);
        }
        dkQueueFlush(queue);

        this->offscreen_width = safe_width;
        this->offscreen_height = safe_height;
        this->offscreen_ready = true;
        return true;
    }

    bool stop_current_playback(bool release_mount_after_stop) {
        bool fully_stopped = true;
        if (this->handle != nullptr) {
            this->suppress_stop_related_errors = true;
            const char* command[] = {"stop", nullptr};
            const int rc = mpv_command_async(
                this->handle,
                static_cast<uint64_t>(AsyncCommandKind::Stop),
                command);
            if (rc < 0) {
                append_debug_log(std::string("[stop] async stop command failed: ") + mpv_error_string(rc));
                fully_stopped = false;
            } else {
                append_debug_log("[stop] async stop command queued");
                fully_stopped = wait_for_stop_completion(std::chrono::milliseconds(300));
                if (!fully_stopped) {
                    append_debug_log("[stop] wait_for_stop_completion timed out");
                }
            }
        }
        this->suppress_stop_related_errors = false;

        this->session_active.store(false);
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(false);
        this->current_render_slot.store(-1);
        this->preferred_track_selection_pending = false;
        this->preferred_track_selection_applied = false;
        this->preferred_track_selection_waiting_for_restart = false;
        this->preferred_track_selection_attempts = 0;
        this->force_audio_track_reapply_for_current_file = false;
        this->rendered_frame_for_current_file.store(false);
        this->playback_started_at_ms.store(0);
        this->playback_restart_at_ms.store(0);
        this->last_render_kick_at_ms.store(0);
        this->last_audio_reapply_at_ms.store(0);
        this->open_requested_at_ms.store(0);
        this->needs_full_reset_before_next_open = true;
        this->active_source_kind = PlaybackSourceKind::Unknown;

        if (release_mount_after_stop && fully_stopped) {
            switchbox::core::switch_smb_mount_release();
        }

        return fully_stopped;
    }

    void process_pending_events(double timeout_seconds = 0.0) {
        if (this->handle == nullptr) {
            return;
        }

        bool first_wait = true;
        while (true) {
            mpv_event* event = mpv_wait_event(this->handle, first_wait ? timeout_seconds : 0.0);
            first_wait = false;
            if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
                break;
            }

            switch (event->event_id) {
                case MPV_EVENT_FILE_LOADED:
                    this->last_error.clear();
                    this->session_active.store(true);
                    this->has_media = true;
                    this->paused.store(false);
                    this->open_requested_at_ms.store(0);
                    this->preferred_track_selection_pending = !this->disable_auto_track_selection_for_current_file;
                    this->preferred_track_selection_applied = this->disable_auto_track_selection_for_current_file;
                    this->preferred_track_selection_waiting_for_restart =
                        !this->disable_auto_track_selection_for_current_file;
                    this->preferred_track_selection_attempts = 0;
                    this->force_audio_track_reapply_for_current_file =
                        !this->disable_auto_track_selection_for_current_file && this->has_loaded_media_before;
                    this->has_loaded_media_before = true;
                    this->rendered_frame_for_current_file.store(false);
                    this->playback_started_at_ms.store(monotonic_milliseconds());
                    this->playback_restart_at_ms.store(0);
                    this->last_render_kick_at_ms.store(0);
                    this->last_audio_reapply_at_ms.store(0);
                    request_render_update();
                    append_debug_log("[event] MPV_EVENT_FILE_LOADED");
                    if (this->disable_auto_track_selection_for_current_file) {
                        append_debug_log("[pref] auto track selection skipped for non-seekable direct_flv");
                    }
                    break;
                case MPV_EVENT_PLAYBACK_RESTART:
                    this->last_error.clear();
                    this->preferred_track_selection_waiting_for_restart = false;
                    this->playback_restart_at_ms.store(monotonic_milliseconds());
                    request_render_update();
                    append_debug_log("[event] MPV_EVENT_PLAYBACK_RESTART");
                    break;
                case MPV_EVENT_END_FILE: {
                    const uint64_t requested_at_ms = this->open_requested_at_ms.load();
                    this->has_media = false;
                    this->paused.store(false);
                    this->session_active.store(false);
                    this->open_requested_at_ms.store(0);
                    this->preferred_track_selection_pending = false;
                    this->preferred_track_selection_applied = false;
                    this->preferred_track_selection_waiting_for_restart = false;
                    this->preferred_track_selection_attempts = 0;
                    this->force_audio_track_reapply_for_current_file = false;
                    this->rendered_frame_for_current_file.store(false);
                    this->playback_started_at_ms.store(0);
                    this->playback_restart_at_ms.store(0);
                    this->last_render_kick_at_ms.store(0);
                    this->last_audio_reapply_at_ms.store(0);
                    this->active_iptv_stream_class = switchbox::core::IptvPreparedStreamClass::Unknown;
                    this->disable_auto_track_selection_for_current_file = false;

                    const auto* end_file = static_cast<mpv_event_end_file*>(event->data);
                    if (end_file != nullptr && end_file->reason == MPV_END_FILE_REASON_ERROR) {
                        if (this->suppress_stop_related_errors) {
                            append_debug_log("[event] MPV_EVENT_END_FILE error suppressed during intentional stop");
                        } else if (end_file->error < 0) {
                            this->last_error = std::string("Playback ended: ") + mpv_error_string(end_file->error);
                            append_debug_log("[event] MPV_EVENT_END_FILE error: " + this->last_error);
                        } else {
                            this->last_error = "Playback ended with an unknown error.";
                            append_debug_log("[event] MPV_EVENT_END_FILE error: " + this->last_error);
                        }
                    } else if (requested_at_ms != 0 && !this->suppress_stop_related_errors) {
                        this->last_error = "Playback ended before media loaded.";
                        append_debug_log("[event] MPV_EVENT_END_FILE normal before FILE_LOADED");
                    } else {
                        append_debug_log("[event] MPV_EVENT_END_FILE normal");
                    }
                    break;
                }
                case MPV_EVENT_COMMAND_REPLY:
                    if (event->error < 0) {
                        const AsyncCommandKind command_kind =
                            async_command_kind_from_reply_userdata(event->reply_userdata);
                        const std::string command_error = mpv_error_string(event->error);

                        if (command_kind == AsyncCommandKind::Seek) {
                            append_debug_log(
                                "[event] MPV_EVENT_COMMAND_REPLY seek error ignored: " + command_error);
                            break;
                        }

                        if (command_kind == AsyncCommandKind::Stop) {
                            if (this->suppress_stop_related_errors) {
                                append_debug_log(
                                    "[event] MPV_EVENT_COMMAND_REPLY stop error suppressed during intentional stop");
                            } else {
                                append_debug_log(
                                    "[event] MPV_EVENT_COMMAND_REPLY stop error ignored: " + command_error);
                            }
                            break;
                        }

                        this->has_media = false;
                        this->session_active.store(false);
                        this->open_requested_at_ms.store(0);
                        this->preferred_track_selection_pending = false;
                        this->preferred_track_selection_applied = false;
                        this->preferred_track_selection_waiting_for_restart = false;
                        this->preferred_track_selection_attempts = 0;
                        this->force_audio_track_reapply_for_current_file = false;
                        this->rendered_frame_for_current_file.store(false);
                        this->playback_started_at_ms.store(0);
                        this->playback_restart_at_ms.store(0);
                        this->last_render_kick_at_ms.store(0);
                        this->last_audio_reapply_at_ms.store(0);
                        this->active_iptv_stream_class = switchbox::core::IptvPreparedStreamClass::Unknown;
                        this->disable_auto_track_selection_for_current_file = false;
                        if (this->suppress_stop_related_errors) {
                            append_debug_log("[event] MPV_EVENT_COMMAND_REPLY error suppressed during intentional stop");
                        } else {
                            this->last_error = std::string("mpv ")
                                + async_command_kind_label(command_kind)
                                + " command failed: "
                                + command_error;
                            append_debug_log("[event] MPV_EVENT_COMMAND_REPLY error: " + this->last_error);
                        }
                    }
                    break;
                case MPV_EVENT_PROPERTY_CHANGE: {
                    const auto* property = static_cast<mpv_event_property*>(event->data);
                    if (property == nullptr || property->name == nullptr) {
                        break;
                    }

                    if (std::strcmp(property->name, "pause") == 0 &&
                        property->format == MPV_FORMAT_FLAG &&
                        property->data != nullptr) {
                        this->paused.store(*static_cast<int*>(property->data) != 0);
                    }
                    break;
                }
                case MPV_EVENT_LOG_MESSAGE: {
                    const auto* message = static_cast<mpv_event_log_message*>(event->data);
                    if (message != nullptr &&
                        message->prefix != nullptr &&
                        message->level != nullptr &&
                        message->text != nullptr) {
                        const std::string prefix = message->prefix;
                        const std::string level = message->level;
                        const std::string text = trim(message->text);
                        const bool relevant_prefix =
                            prefix == "ffmpeg" ||
                            prefix == "stream" ||
                            prefix == "demux" ||
                            prefix == "cache" ||
                            prefix == "cplayer";
                        const bool important_level =
                            level == "fatal" ||
                            level == "error" ||
                            level == "warn" ||
                            (this->active_source_kind == PlaybackSourceKind::Iptv && level == "info");

                        if (relevant_prefix && important_level && !text.empty()) {
                            append_debug_log("[mpv][" + prefix + "][" + level + "] " + text);
                        }

                        if (((level == "fatal" || level == "error") &&
                             (prefix == "ffmpeg" || prefix == "stream" || prefix == "demux")) ||
                            should_capture_iptv_warning_as_error(this->active_source_kind, prefix, level, text)) {
                            if (this->suppress_stop_related_errors) {
                                append_debug_log("[event] log error suppressed during intentional stop");
                            } else {
                                this->last_error = text;
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    bool initialize(std::string& error_message) {
        if (this->ready) {
            return true;
        }

        this->handle = mpv_create();
        if (this->handle == nullptr) {
            error_message = "mpv_create failed.";
            return false;
        }

        mpv_set_option_string(this->handle, "config", "yes");
        mpv_set_option_string(this->handle, "terminal", "no");
        mpv_set_option_string(this->handle, "msg-level", "all=warn");
        mpv_set_option_string(this->handle, "keep-open", "yes");
        mpv_set_option_string(this->handle, "idle", "yes");
        mpv_set_option_string(this->handle, "vo", "libmpv");
        mpv_set_option_string(this->handle, "ao", "hos");
        mpv_set_option_string(this->handle, "user-agent", build_default_iptv_user_agent().c_str());
        mpv_set_option_string(this->handle, "reset-on-next-file", "aid,vid,sid,pause,speed,video-rotate");
        mpv_set_option_string(this->handle, "hwdec", "nvtegra");
        mpv_set_option_string(this->handle, "hwdec-codecs", kHwdecCodecs);
        mpv_set_option_string(this->handle, "pause", "no");
        mpv_set_option_string(this->handle, "audio-display", "no");
        mpv_set_option_string(this->handle, "vd-lavc-dr", "no");
        mpv_set_option_string(this->handle, "vd-lavc-threads", "4");
        mpv_set_option_string(this->handle, "correct-downscaling", "no");
        mpv_set_option_string(this->handle, "linear-downscaling", "no");
        mpv_set_option_string(this->handle, "sigmoid-upscaling", "no");
        mpv_set_option_string(this->handle, "scale", "bilinear");
        mpv_set_option_string(this->handle, "dscale", "bilinear");
        mpv_set_option_string(this->handle, "cscale", "bilinear");
        mpv_set_option_string(this->handle, "tscale", "oversample");
        mpv_set_option_string(this->handle, "dither-depth", "no");
        mpv_set_option_string(this->handle, "deband", "no");
        mpv_set_option_string(this->handle, "hdr-compute-peak", "no");
        mpv_set_option_string(this->handle, "demuxer-seekable-cache", "yes");
        mpv_set_option_string(this->handle, "demuxer-lavf-analyzeduration", "0.4");
        mpv_set_option_string(this->handle, "demuxer-lavf-probescore", "24");
        mpv_set_option_string(
            this->handle,
            "demuxer-readahead-secs",
            std::to_string(std::max(0, switchbox::core::AppConfigStore::current().general.demux_cache_sec)).c_str());

        configure_embedded_subtitle_fonts(this->handle);

        if (mpv_initialize(this->handle) < 0) {
            error_message = "mpv_initialize failed.";
            return false;
        }

        if (!register_memory_text_protocol(error_message)) {
            mpv_terminate_destroy(this->handle);
            this->handle = nullptr;
            return false;
        }

        mpv_request_log_messages(this->handle, "info");
        mpv_observe_property(this->handle, 0, "pause", MPV_FORMAT_FLAG);
        this->ready = true;
        return true;
    }

    bool register_memory_text_protocol(std::string& error_message) {
        if (this->handle == nullptr) {
            error_message = "mpv handle is not initialized.";
            return false;
        }
        if (this->memory_text_protocol_registered) {
            return true;
        }

        const int rc =
            mpv_stream_cb_add_ro(this->handle, kMemoryTextProtocol, nullptr, &memory_text_stream_open_ro);
        if (rc < 0) {
            error_message = std::string("mpv_stream_cb_add_ro failed: ") + mpv_error_string(rc);
            return false;
        }

        this->memory_text_protocol_registered = true;
        append_debug_log("[mem] protocol registered");
        return true;
    }

    bool ensure_deko3d_context(DkDevice device, DkQueue queue, int width, int height, std::string& error_message) {
        if (this->handle == nullptr) {
            error_message = "mpv handle is not initialized.";
            return false;
        }

        if (this->context == nullptr) {
            int advanced_control = 1;
            mpv_deko3d_init_params deko3d_init_params {device};
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_DEKO3D)},
                {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &deko3d_init_params},
                {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };

            if (mpv_render_context_create(&this->context, this->handle, params) < 0) {
                error_message = "mpv_render_context_create failed.";
                return false;
            }

            mpv_render_context_set_update_callback(this->context, &SwitchMpvPlayer::mpv_render_update, this);
        }

        if (!ensure_render_buffers(device, queue, width, height, error_message)) {
            return false;
        }

        ensure_render_thread();
        return true;
    }

    SwitchMpvPlayer() = default;

    ~SwitchMpvPlayer() {
        shutdown();
    }

    mpv_handle* handle = nullptr;
    mpv_render_context* context = nullptr;
    bool ready = false;
    bool memory_text_protocol_registered = false;
    bool has_media = false;
    std::atomic<bool> session_active = false;
    std::atomic<bool> paused = false;
    std::atomic<bool> frame_dirty = false;
    std::atomic<int> current_render_slot = -1;
    double playback_speed = 1.0;
    int video_rotation_degrees = 0;
    int playback_volume = 80;
    bool preferred_track_selection_pending = false;
    bool preferred_track_selection_applied = false;
    bool preferred_track_selection_waiting_for_restart = false;
    int preferred_track_selection_attempts = 0;
    switchbox::core::IptvPreparedStreamClass active_iptv_stream_class =
        switchbox::core::IptvPreparedStreamClass::Unknown;
    bool disable_auto_track_selection_for_current_file = false;
    bool force_audio_track_reapply_for_current_file = false;
    bool has_loaded_media_before = false;
    bool suppress_stop_related_errors = false;
    std::atomic<bool> rendered_frame_for_current_file = false;
    std::atomic<uint64_t> playback_started_at_ms = 0;
    std::atomic<uint64_t> playback_restart_at_ms = 0;
    std::atomic<uint64_t> last_render_kick_at_ms = 0;
    std::atomic<uint64_t> last_audio_reapply_at_ms = 0;
    std::atomic<uint64_t> open_requested_at_ms = 0;
    bool needs_full_reset_before_next_open = false;
    PlaybackSourceKind active_source_kind = PlaybackSourceKind::Unknown;
    std::string last_error;
    DkDevice deko3d_device = nullptr;
    DkQueue deko3d_queue = nullptr;
    bool offscreen_ready = false;
    int offscreen_width = 0;
    int offscreen_height = 0;
    std::array<dk::Image, kMpvImageCount> mpv_images {};
    dk::UniqueMemBlock mpv_images_memblock;
    std::array<dk::Fence, kMpvImageCount> mpv_copy_fences {};
    dk::UniqueCmdBuf present_cmd_buf;
    dk::UniqueMemBlock present_cmd_memblock;
    std::array<dk::Fence, kPresentCmdBufSlices> present_cmd_fences {};
    unsigned present_cmd_slice = 0;
    std::mutex render_mutex;
    std::mutex mpv_redraw_mutex;
    std::condition_variable mpv_redraw_condvar;
    int mpv_redraw_count = 0;
    std::jthread mpv_render_thread;
};

}  // namespace

bool switch_mpv_backend_available() {
    return true;
}

std::string switch_mpv_backend_reason() {
    return {};
}

void switch_mpv_begin_debug_log_session() {
    begin_debug_log_session_impl();
}

void switch_mpv_append_debug_log_note(const std::string& message) {
    append_debug_log("[note] " + message);
}

std::string switch_mpv_register_memory_text_stream(
    std::string extension,
    std::string data) {
    return SwitchMpvPlayer::instance().register_memory_text_stream(std::move(extension), std::move(data));
}

bool switch_mpv_open(const PlaybackTarget& target, std::string& error_message) {
    return SwitchMpvPlayer::instance().open(target, error_message);
}

void switch_mpv_stop() {
    SwitchMpvPlayer::instance().stop();
}

void switch_mpv_shutdown() {
    SwitchMpvPlayer::instance().shutdown();
}

#if defined(__SWITCH__)
bool switch_mpv_prepare_renderer_for_switch(
    DkDevice device,
    DkQueue queue,
    int width,
    int height,
    std::string& error_message) {
    return SwitchMpvPlayer::instance().prepare_renderer_for_switch(device, queue, width, height, error_message);
}
#endif

bool switch_mpv_session_active() {
    return SwitchMpvPlayer::instance().is_session_active();
}

bool switch_mpv_has_media() {
    return SwitchMpvPlayer::instance().has_active_media();
}

bool switch_mpv_has_rendered_video_frame() {
    return SwitchMpvPlayer::instance().has_rendered_frame_for_current_file();
}

void switch_mpv_toggle_pause() {
    SwitchMpvPlayer::instance().toggle_pause();
}

bool switch_mpv_seek_relative_seconds(double delta_seconds) {
    return SwitchMpvPlayer::instance().seek_relative_seconds(delta_seconds);
}

bool switch_mpv_seek_absolute_seconds(double target_seconds) {
    return SwitchMpvPlayer::instance().seek_absolute_seconds(target_seconds);
}

bool switch_mpv_set_speed(double speed) {
    return SwitchMpvPlayer::instance().set_speed(speed);
}

double switch_mpv_get_speed() {
    return SwitchMpvPlayer::instance().get_speed();
}

bool switch_mpv_rotate_clockwise_90() {
    return SwitchMpvPlayer::instance().rotate_clockwise_90();
}

int switch_mpv_get_video_rotation_degrees() {
    return SwitchMpvPlayer::instance().get_video_rotation_degrees();
}

bool switch_mpv_set_volume(int volume) {
    return SwitchMpvPlayer::instance().set_volume(volume);
}

int switch_mpv_get_volume() {
    return SwitchMpvPlayer::instance().get_volume();
}

int64_t switch_mpv_get_transfer_speed_bytes_per_second() {
    return SwitchMpvPlayer::instance().get_transfer_speed_bytes_per_second();
}

std::uint64_t switch_mpv_get_playback_restart_at_ms() {
    return SwitchMpvPlayer::instance().get_playback_restart_at_ms();
}

std::vector<MpvTrackOption> switch_mpv_list_audio_tracks() {
    return SwitchMpvPlayer::instance().list_audio_tracks();
}

std::vector<MpvTrackOption> switch_mpv_list_subtitle_tracks() {
    return SwitchMpvPlayer::instance().list_subtitle_tracks();
}

bool switch_mpv_set_audio_track(int id, std::string& error_message) {
    return SwitchMpvPlayer::instance().set_audio_track(id, error_message);
}

bool switch_mpv_set_subtitle_track(int id, std::string& error_message) {
    return SwitchMpvPlayer::instance().set_subtitle_track(id, error_message);
}

double switch_mpv_get_position_seconds() {
    return SwitchMpvPlayer::instance().get_position_seconds();
}

double switch_mpv_get_duration_seconds() {
    return SwitchMpvPlayer::instance().get_duration_seconds();
}

bool switch_mpv_is_paused() {
    return SwitchMpvPlayer::instance().is_paused();
}

std::string switch_mpv_consume_last_error() {
    return SwitchMpvPlayer::instance().consume_last_error();
}

#if defined(__SWITCH__)
bool switch_mpv_render_deko3d_frame(
    DkDevice device,
    DkQueue queue,
    DkImage* texture,
    int width,
    int height) {
    return SwitchMpvPlayer::instance().render_deko3d_frame(
        device,
        queue,
        texture,
        std::max(1, width),
        std::max(1, height));
}
#endif

void switch_mpv_report_render_swap() {
    SwitchMpvPlayer::instance().report_render_swap();
}

#else

bool switch_mpv_backend_available() {
    return false;
}

std::string switch_mpv_backend_reason() {
    return "Custom Switch libmpv/deko3d build not found under devkitPro tmp/switchbox-portlibs.";
}

void switch_mpv_begin_debug_log_session() {
}

void switch_mpv_append_debug_log_note(const std::string&) {
}

std::string switch_mpv_register_memory_text_stream(std::string, std::string) {
    return {};
}

bool switch_mpv_open(const PlaybackTarget&, std::string& error_message) {
    error_message = switch_mpv_backend_reason();
    return false;
}

void switch_mpv_stop() {
}

void switch_mpv_shutdown() {
}

#if defined(__SWITCH__)
bool switch_mpv_prepare_renderer_for_switch(
    DkDevice,
    DkQueue,
    int,
    int,
    std::string& error_message) {
    error_message = switch_mpv_backend_reason();
    return false;
}
#endif

bool switch_mpv_session_active() {
    return false;
}

bool switch_mpv_has_media() {
    return false;
}

bool switch_mpv_has_rendered_video_frame() {
    return false;
}

void switch_mpv_toggle_pause() {
}

bool switch_mpv_seek_relative_seconds(double) {
    return false;
}

bool switch_mpv_seek_absolute_seconds(double) {
    return false;
}

bool switch_mpv_set_speed(double) {
    return false;
}

double switch_mpv_get_speed() {
    return 1.0;
}

bool switch_mpv_rotate_clockwise_90() {
    return false;
}

int switch_mpv_get_video_rotation_degrees() {
    return 0;
}

bool switch_mpv_set_volume(int) {
    return false;
}

int switch_mpv_get_volume() {
    return 100;
}

int64_t switch_mpv_get_transfer_speed_bytes_per_second() {
    return 0;
}

std::uint64_t switch_mpv_get_playback_restart_at_ms() {
    return 0;
}

std::vector<MpvTrackOption> switch_mpv_list_audio_tracks() {
    return {};
}

std::vector<MpvTrackOption> switch_mpv_list_subtitle_tracks() {
    return {};
}

bool switch_mpv_set_audio_track(int, std::string& error_message) {
    error_message = switch_mpv_backend_reason();
    return false;
}

bool switch_mpv_set_subtitle_track(int, std::string& error_message) {
    error_message = switch_mpv_backend_reason();
    return false;
}

double switch_mpv_get_position_seconds() {
    return 0.0;
}

double switch_mpv_get_duration_seconds() {
    return 0.0;
}

bool switch_mpv_is_paused() {
    return false;
}

std::string switch_mpv_consume_last_error() {
    return {};
}

#if defined(__SWITCH__)
bool switch_mpv_render_deko3d_frame(
    DkDevice,
    DkQueue,
    DkImage*,
    int,
    int) {
    return false;
}
#endif

void switch_mpv_report_render_swap() {
}

#endif

}  // namespace switchbox::core
