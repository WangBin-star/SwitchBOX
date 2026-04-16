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
#include <thread>
#include <utility>
#include <vector>

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)
#include <deko3d.hpp>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_dk3d.h>
#include <switch.h>
#endif

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/smb2_mount_fs.hpp"

namespace switchbox::core {

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)

namespace {

void append_debug_log(const std::string& message) {
    (void)message;
}

uint16_t read_be16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
}

uint32_t read_be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string utf8_from_codepoint(uint32_t codepoint) {
    std::string output;
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return output;
}

std::string decode_utf16be(const uint8_t* data, size_t size) {
    std::string output;
    for (size_t offset = 0; offset + 1 < size; offset += 2) {
        uint32_t codepoint = read_be16(data + offset);
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && offset + 3 < size) {
            const uint32_t low = read_be16(data + offset + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
                offset += 2;
            }
        }
        output += utf8_from_codepoint(codepoint);
    }
    return trim(output);
}

std::string decode_name_record(
    uint16_t platform_id,
    const uint8_t* data,
    size_t size) {
    if (platform_id == 0 || platform_id == 3) {
        return decode_utf16be(data, size);
    }

    return trim(std::string(reinterpret_cast<const char*>(data), size));
}

std::string extract_font_family_name_from_sfnt_face(
    const uint8_t* data,
    size_t size,
    size_t face_offset) {
    if (face_offset + 12 > size) {
        return {};
    }

    const uint8_t* face = data + face_offset;
    const size_t face_size = size - face_offset;
    const uint16_t table_count = read_be16(face + 4);
    if (12 + static_cast<size_t>(table_count) * 16 > face_size) {
        return {};
    }

    size_t name_table_offset = 0;
    size_t name_table_length = 0;
    for (uint16_t index = 0; index < table_count; ++index) {
        const uint8_t* record = face + 12 + static_cast<size_t>(index) * 16;
        if (std::memcmp(record, "name", 4) != 0) {
            continue;
        }

        name_table_offset = static_cast<size_t>(read_be32(record + 8));
        name_table_length = static_cast<size_t>(read_be32(record + 12));
        break;
    }

    if (name_table_offset == 0 || name_table_offset + name_table_length > face_size || name_table_length < 6) {
        return {};
    }

    const uint8_t* name_table = face + name_table_offset;
    const uint16_t record_count = read_be16(name_table + 2);
    const uint16_t string_offset = read_be16(name_table + 4);
    if (6 + static_cast<size_t>(record_count) * 12 > name_table_length || string_offset > name_table_length) {
        return {};
    }

    int best_score = std::numeric_limits<int>::min();
    std::string best_name;

    for (uint16_t index = 0; index < record_count; ++index) {
        const uint8_t* record = name_table + 6 + static_cast<size_t>(index) * 12;
        const uint16_t platform_id = read_be16(record + 0);
        const uint16_t language_id = read_be16(record + 4);
        const uint16_t name_id = read_be16(record + 6);
        const uint16_t text_length = read_be16(record + 8);
        const uint16_t text_offset = read_be16(record + 10);

        if (name_id != 16 && name_id != 1) {
            continue;
        }
        if (static_cast<size_t>(string_offset) + text_offset + text_length > name_table_length) {
            continue;
        }

        const uint8_t* text = name_table + string_offset + text_offset;
        const std::string decoded = decode_name_record(platform_id, text, text_length);
        if (decoded.empty()) {
            continue;
        }

        int score = (name_id == 16 ? 100 : 50);
        if (platform_id == 3) {
            score += 40;
        } else if (platform_id == 0) {
            score += 30;
        } else if (platform_id == 1) {
            score += 20;
        }
        if (language_id == 0x0409 || language_id == 0) {
            score += 10;
        }

        if (score > best_score) {
            best_score = score;
            best_name = decoded;
        }
    }

    return best_name;
}

std::string extract_font_family_name_from_sfnt(const void* raw_data, size_t size) {
    if (raw_data == nullptr || size < 12) {
        return {};
    }

    const auto* data = static_cast<const uint8_t*>(raw_data);
    if (std::memcmp(data, "ttcf", 4) == 0) {
        if (size < 12) {
            return {};
        }
        const uint32_t face_count = read_be32(data + 8);
        for (uint32_t index = 0; index < face_count; ++index) {
            const size_t offset_pos = 12 + static_cast<size_t>(index) * 4;
            if (offset_pos + 4 > size) {
                break;
            }
            const size_t face_offset = static_cast<size_t>(read_be32(data + offset_pos));
            const std::string family_name = extract_font_family_name_from_sfnt_face(data, size, face_offset);
            if (!family_name.empty()) {
                return family_name;
            }
        }
        return {};
    }

    return extract_font_family_name_from_sfnt_face(data, size, 0);
}

bool write_binary_file(
    const std::filesystem::path& path,
    const void* data,
    size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return output.good();
}

struct SwitchSubtitleFontCache {
    bool attempted = false;
    bool ready = false;
    std::filesystem::path directory;
    std::string family_name;
};

SwitchSubtitleFontCache& subtitle_font_cache() {
    static SwitchSubtitleFontCache cache;
    return cache;
}

bool prepare_switch_subtitle_fonts(std::string& font_directory, std::string& font_family) {
    auto& cache = subtitle_font_cache();
    if (cache.attempted) {
        font_directory = cache.directory.string();
        font_family = cache.family_name;
        return cache.ready;
    }

    cache.attempted = true;

    const auto font_directory_path = AppConfigStore::paths().base_directory / ".switchbox-mpv-fonts";
    std::error_code error;
    std::filesystem::create_directories(font_directory_path, error);
    if (error) {
        return false;
    }

    struct FontDumpRequest {
        PlSharedFontType type;
        const char* file_name;
        bool use_for_family_name;
    };

    const FontDumpRequest requests[] = {
        {PlSharedFontType_Standard, "standard.ttf", false},
        {PlSharedFontType_ChineseSimplified, "zh-Hans.ttf", true},
        {PlSharedFontType_ExtChineseSimplified, "zh-Hans-ext.ttf", false},
    };

    for (const auto& request : requests) {
        PlFontData font_data {};
        if (R_FAILED(plGetSharedFontByType(&font_data, request.type)) || font_data.address == nullptr || font_data.size == 0) {
            continue;
        }

        if (!write_binary_file(font_directory_path / request.file_name, font_data.address, font_data.size)) {
            continue;
        }

        if (request.use_for_family_name && cache.family_name.empty()) {
            cache.family_name = extract_font_family_name_from_sfnt(font_data.address, font_data.size);
        }
    }

    if (cache.family_name.empty()) {
        return false;
    }

    cache.directory = font_directory_path;
    cache.ready = true;
    font_directory = cache.directory.string();
    font_family = cache.family_name;
    return true;
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
        if (this->session_active.load() || this->has_media || !is_idle_active()) {
            if (!stop_current_playback(false)) {
                error_message = "Previous playback session did not stop cleanly.";
                return false;
            }
        }

        if (this->needs_full_reset_before_next_open) {
            reset_backend_core_state();
        }

        if (!initialize(error_message)) {
            return false;
        }

        if (target.source_kind == PlaybackSourceKind::Smb && target.smb_locator.has_value()) {
            if (!switchbox::core::switch_smb_mount_resolve_playback_path(
                    target.smb_locator.value(),
                    locator,
                    error_message)) {
                this->last_error = error_message;
                append_debug_log("[open] SMB mount failed: " + error_message);
                return false;
            }
        } else {
            switchbox::core::switch_smb_mount_release();
            locator = pick_locator(target);
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

        append_debug_log("[open] locator=" + locator);

        this->session_active.store(true);
        apply_runtime_preferences();
        (void)apply_video_rotation_degrees(0, false);

        const char* command[] = {"loadfile", locator.c_str(), "replace", nullptr};
        const int rc = mpv_command_async(this->handle, 0, command);
        if (rc < 0) {
            this->session_active.store(false);
            error_message = mpv_error_string(rc);
            append_debug_log(std::string("[open] mpv_command_async failed: ") + error_message);
            return false;
        }

        this->set_speed(1.0);
        this->set_volume(this->playback_volume);
        process_pending_events();
        return true;
    }

    void stop() {
        (void)stop_current_playback(false);
        reset_backend_core_state();
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
        const int rc = mpv_command_async(this->handle, 0, command);
        if (rc < 0) {
            this->last_error = std::string("Seek failed: ") + mpv_error_string(rc);
            append_debug_log("[input] seek failed: " + this->last_error);
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
        if (!this->force_audio_track_reapply_for_current_file ||
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
            mpv_command_string(this->handle, "stop");
            fully_stopped = wait_for_stop_completion(std::chrono::milliseconds(1500));
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
        this->needs_full_reset_before_next_open = true;

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
                    this->session_active.store(true);
                    this->has_media = true;
                    this->paused.store(false);
                    this->preferred_track_selection_pending = true;
                    this->preferred_track_selection_applied = false;
                    this->preferred_track_selection_waiting_for_restart = true;
                    this->preferred_track_selection_attempts = 0;
                    this->force_audio_track_reapply_for_current_file = this->has_loaded_media_before;
                    this->has_loaded_media_before = true;
                    this->rendered_frame_for_current_file.store(false);
                    this->playback_started_at_ms.store(monotonic_milliseconds());
                    this->playback_restart_at_ms.store(0);
                    this->last_render_kick_at_ms.store(0);
                    this->last_audio_reapply_at_ms.store(0);
                    request_render_update();
                    append_debug_log("[event] MPV_EVENT_FILE_LOADED");
                    break;
                case MPV_EVENT_PLAYBACK_RESTART:
                    this->preferred_track_selection_waiting_for_restart = false;
                    this->playback_restart_at_ms.store(monotonic_milliseconds());
                    request_render_update();
                    append_debug_log("[event] MPV_EVENT_PLAYBACK_RESTART");
                    break;
                case MPV_EVENT_END_FILE: {
                    this->has_media = false;
                    this->paused.store(false);
                    this->session_active.store(false);
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
                    } else {
                        append_debug_log("[event] MPV_EVENT_END_FILE normal");
                    }
                    break;
                }
                case MPV_EVENT_COMMAND_REPLY:
                    if (event->error < 0) {
                        this->has_media = false;
                        this->session_active.store(false);
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
                        if (this->suppress_stop_related_errors) {
                            append_debug_log("[event] MPV_EVENT_COMMAND_REPLY error suppressed during intentional stop");
                        } else {
                            this->last_error = std::string("mpv command failed: ") + mpv_error_string(event->error);
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
                        message->text != nullptr &&
                        std::strcmp(message->prefix, "ffmpeg") == 0 &&
                        std::strcmp(message->level, "error") == 0) {
                        if (this->suppress_stop_related_errors) {
                            append_debug_log("[event] ffmpeg error suppressed during intentional stop");
                        } else {
                            this->last_error = message->text;
                            append_debug_log(std::string("[event] ffmpeg error: ") + this->last_error);
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
        mpv_set_option_string(
            this->handle,
            "demuxer-readahead-secs",
            std::to_string(std::max(0, switchbox::core::AppConfigStore::current().general.demux_cache_sec)).c_str());

        std::string subtitle_font_directory;
        std::string subtitle_font_family;
        if (prepare_switch_subtitle_fonts(subtitle_font_directory, subtitle_font_family)) {
            mpv_set_option_string(this->handle, "sub-fonts-dir", subtitle_font_directory.c_str());
            mpv_set_option_string(this->handle, "sub-font", subtitle_font_family.c_str());
            mpv_set_option_string(this->handle, "osd-font", subtitle_font_family.c_str());
            mpv_set_option_string(
                this->handle,
                "sub-ass-force-style",
                ("FontName=" + subtitle_font_family).c_str());
        }

        if (mpv_initialize(this->handle) < 0) {
            error_message = "mpv_initialize failed.";
            return false;
        }

        mpv_observe_property(this->handle, 0, "pause", MPV_FORMAT_FLAG);
        this->ready = true;
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
    bool force_audio_track_reapply_for_current_file = false;
    bool has_loaded_media_before = false;
    bool suppress_stop_related_errors = false;
    std::atomic<bool> rendered_frame_for_current_file = false;
    std::atomic<uint64_t> playback_started_at_ms = 0;
    std::atomic<uint64_t> playback_restart_at_ms = 0;
    std::atomic<uint64_t> last_render_kick_at_ms = 0;
    std::atomic<uint64_t> last_audio_reapply_at_ms = 0;
    bool needs_full_reset_before_next_open = false;
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

void switch_mpv_toggle_pause() {
    SwitchMpvPlayer::instance().toggle_pause();
}

bool switch_mpv_seek_relative_seconds(double delta_seconds) {
    return SwitchMpvPlayer::instance().seek_relative_seconds(delta_seconds);
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

void switch_mpv_toggle_pause() {
}

bool switch_mpv_seek_relative_seconds(double) {
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
