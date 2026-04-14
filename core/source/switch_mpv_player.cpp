#include "switchbox/core/switch_mpv_player.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>
#include <sstream>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)
#include <fcntl.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <smb2/libsmb2.h>
#include <smb2/smb2.h>
#endif

#include "switchbox/core/app_config.hpp"

namespace switchbox::core {

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)

namespace {

std::filesystem::path runtime_base_directory() {
    const auto& paths = switchbox::core::AppConfigStore::paths();
    std::filesystem::path base = paths.config_file.parent_path();
    if (base.empty()) {
        base = std::filesystem::path("sdmc:/switch/SwitchBOX");
    }

    std::error_code error;
    std::filesystem::create_directories(base, error);
    return base;
}

std::string path_string(const std::filesystem::path& path) {
    return path.generic_string();
}

void append_debug_log(const std::string& message) {
    const std::filesystem::path log_path = runtime_base_directory() / "playback-debug.log";
    FILE* file = std::fopen(path_string(log_path).c_str(), "a");
    if (file == nullptr) {
        return;
    }

    std::fputs(message.c_str(), file);
    std::fputs("\n", file);
    std::fclose(file);
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string trim_network_component(std::string value) {
    value = trim(std::move(value));

    while (!value.empty() && (value.front() == '/' || value.front() == '\\')) {
        value.erase(value.begin());
    }

    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }

    return value;
}

std::vector<std::string> split_relative_segments(std::string_view raw_path) {
    std::vector<std::string> segments;
    std::string current;

    const auto flush = [&segments, &current]() {
        std::string segment = trim_network_component(current);
        current.clear();

        if (segment.empty() || segment == ".") {
            return;
        }

        if (segment == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
            return;
        }

        segments.push_back(std::move(segment));
    };

    for (const char character : raw_path) {
        if (character == '/' || character == '\\') {
            flush();
            continue;
        }

        current.push_back(character);
    }

    flush();
    return segments;
}

std::string join_segments(const std::vector<std::string>& segments) {
    std::string path;
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            path += "/";
        }
        path += segments[index];
    }
    return path;
}

struct ParsedShareTarget {
    std::string share_name;
    std::string initial_relative_path;
};

ParsedShareTarget parse_share_target(const std::string& raw_share) {
    const auto segments = split_relative_segments(raw_share);
    if (segments.empty()) {
        return {};
    }

    ParsedShareTarget parsed;
    parsed.share_name = segments.front();
    if (segments.size() > 1) {
        parsed.initial_relative_path =
            join_segments(std::vector<std::string>(segments.begin() + 1, segments.end()));
    }
    return parsed;
}

std::string build_smb2_file_path(const PlaybackTarget::SmbLocator& locator) {
    const ParsedShareTarget share_target = parse_share_target(locator.share);
    if (share_target.share_name.empty()) {
        return {};
    }

    std::vector<std::string> segments = split_relative_segments(share_target.initial_relative_path);
    const auto relative = split_relative_segments(locator.relative_path);
    segments.insert(segments.end(), relative.begin(), relative.end());
    return join_segments(segments);
}

struct Smb2ContextDeleter {
    void operator()(smb2_context* context) const {
        if (context != nullptr) {
            smb2_destroy_context(context);
        }
    }
};

struct Smb2FileHandleDeleter {
    smb2_context* context = nullptr;

    void operator()(smb2fh* handle) const {
        if (context != nullptr && handle != nullptr) {
            smb2_close(context, handle);
        }
    }
};

bool cache_smb_media_to_local(
    const PlaybackTarget::SmbLocator& smb_locator,
    std::string& local_path,
    std::string& error_message) {
    const std::string host = trim_network_component(smb_locator.host);
    const ParsedShareTarget share_target = parse_share_target(smb_locator.share);
    const std::string smb_file_path = build_smb2_file_path(smb_locator);
    if (host.empty() || share_target.share_name.empty() || smb_file_path.empty()) {
        error_message = "SMB playback target is missing host/share/path.";
        return false;
    }

    const std::filesystem::path cache_path = runtime_base_directory() / "switchbox-playback-cache.bin";
    local_path = path_string(cache_path);

    std::unique_ptr<smb2_context, Smb2ContextDeleter> smb2(smb2_init_context());
    if (!smb2) {
        error_message = "Failed to initialize libsmb2 context.";
        return false;
    }

    smb2_set_timeout(smb2.get(), 10);
    smb2_set_security_mode(smb2.get(), SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (!smb_locator.username.empty()) {
        smb2_set_user(smb2.get(), smb_locator.username.c_str());
    }
    if (!smb_locator.password.empty()) {
        smb2_set_password(smb2.get(), smb_locator.password.c_str());
    }

    const int connect_result =
        smb2_connect_share(
            smb2.get(),
            host.c_str(),
            share_target.share_name.c_str(),
            smb_locator.username.empty() ? nullptr : smb_locator.username.c_str());
    if (connect_result < 0) {
        error_message = smb2_get_error(smb2.get());
        return false;
    }

    std::unique_ptr<smb2fh, Smb2FileHandleDeleter> file_handle(
        smb2_open(smb2.get(), smb_file_path.c_str(), O_RDONLY),
        Smb2FileHandleDeleter{smb2.get()});
    if (!file_handle) {
        error_message = smb2_get_error(smb2.get());
        smb2_disconnect_share(smb2.get());
        return false;
    }

    FILE* output = std::fopen(local_path.c_str(), "wb");
    if (output == nullptr) {
        error_message = "Unable to open local cache file for SMB playback.";
        smb2_disconnect_share(smb2.get());
        return false;
    }

    constexpr uint32_t kBufferSize = 256 * 1024;
    std::vector<uint8_t> buffer(kBufferSize);

    while (true) {
        const int bytes_read = smb2_read(smb2.get(), file_handle.get(), buffer.data(), kBufferSize);
        if (bytes_read < 0) {
            error_message = smb2_get_error(smb2.get());
            std::fclose(output);
            smb2_disconnect_share(smb2.get());
            return false;
        }

        if (bytes_read == 0) {
            break;
        }

        const size_t bytes_written = std::fwrite(buffer.data(), 1, static_cast<size_t>(bytes_read), output);
        if (bytes_written != static_cast<size_t>(bytes_read)) {
            error_message = "Failed to write SMB cache file.";
            std::fclose(output);
            smb2_disconnect_share(smb2.get());
            return false;
        }
    }

    std::fclose(output);
    smb2_disconnect_share(smb2.get());
    return true;
}

std::string pick_locator(const PlaybackTarget& target) {
    if (!target.primary_locator.empty()) {
        return target.primary_locator;
    }

    return target.fallback_locator;
}

class SwitchMpvPlayer {
public:
    static SwitchMpvPlayer& instance() {
        static SwitchMpvPlayer player;
        return player;
    }

    bool open(const PlaybackTarget& target, std::string& error_message) {
        remove_cache_file();

        std::string locator = pick_locator(target);
        if (target.source_kind == PlaybackSourceKind::Smb && target.smb_locator.has_value()) {
            std::string cache_error;
            std::string local_cache_locator;
            if (cache_smb_media_to_local(target.smb_locator.value(), local_cache_locator, cache_error)) {
                locator = local_cache_locator;
                this->cache_file_path = std::filesystem::path(local_cache_locator);
                append_debug_log("[open] SMB cached to local file: " + locator);
            } else {
                append_debug_log("[open] SMB cache fallback failed: " + cache_error);
                if (!cache_error.empty()) {
                    this->last_error = cache_error;
                }
            }
        }

        if (locator.empty()) {
            error_message = "The selected target does not expose an openable locator yet.";
            return false;
        }

        if (!initialize(error_message)) {
            return false;
        }

        this->last_error.clear();
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(true);
        this->playback_speed = 1.0;
        this->playback_volume = std::clamp(switchbox::core::AppConfigStore::current().general.player_volume, 0, 100);

        append_debug_log("[open] locator=" + locator);

        mpv_command_string(this->handle, "stop");
        process_pending_events();
        this->session_active.store(true);

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
        if (this->handle != nullptr) {
            mpv_command_string(this->handle, "stop");
            process_pending_events();
        }

        this->session_active.store(false);
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(false);
        remove_cache_file();
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

    bool render_rgba_frame(
        int width,
        int height,
        const std::uint8_t** rgba_data,
        size_t* rgba_size,
        int* rgba_stride) {
        process_pending_events();

        if (this->context == nullptr) {
            return false;
        }

        ensure_buffer(width, height);
        (void)mpv_render_context_update(this->context);
        if (this->session_active.load() || this->has_media) {
            render_frame();
        }
        this->frame_dirty.store(false);

        if (rgba_data != nullptr) {
            *rgba_data = this->pixels.empty() ? nullptr : this->pixels.data();
        }

        if (rgba_size != nullptr) {
            *rgba_size = this->pixels.size();
        }

        if (rgba_stride != nullptr) {
            *rgba_stride = static_cast<int>(this->stride);
        }

        return !this->pixels.empty();
    }

    std::string consume_last_error() {
        std::string value = this->last_error;
        this->last_error.clear();
        return value;
    }

private:
    void remove_cache_file() {
        if (this->cache_file_path.empty()) {
            return;
        }

        std::error_code error;
        std::filesystem::remove(this->cache_file_path, error);
        this->cache_file_path.clear();
    }

    static void mpv_render_update(void* context) {
        auto* self = static_cast<SwitchMpvPlayer*>(context);
        if (self != nullptr) {
            self->frame_dirty.store(true);
        }
    }

    void process_pending_events() {
        if (this->handle == nullptr) {
            return;
        }

        while (true) {
            mpv_event* event = mpv_wait_event(this->handle, 0);
            if (event == nullptr || event->event_id == MPV_EVENT_NONE) {
                break;
            }

            switch (event->event_id) {
                case MPV_EVENT_FILE_LOADED:
                    this->session_active.store(true);
                    this->has_media = true;
                    this->paused.store(false);
                    this->frame_dirty.store(true);
                    append_debug_log("[event] MPV_EVENT_FILE_LOADED");
                    break;
                case MPV_EVENT_END_FILE: {
                    this->has_media = false;
                    this->paused.store(false);
                    this->session_active.store(false);

                    const auto* end_file = static_cast<mpv_event_end_file*>(event->data);
                    if (end_file != nullptr && end_file->reason == MPV_END_FILE_REASON_ERROR) {
                        if (end_file->error < 0) {
                            this->last_error = std::string("Playback ended: ") + mpv_error_string(end_file->error);
                        } else {
                            this->last_error = "Playback ended with an unknown error.";
                        }
                        append_debug_log("[event] MPV_EVENT_END_FILE error: " + this->last_error);
                    } else {
                        append_debug_log("[event] MPV_EVENT_END_FILE normal");
                    }
                    break;
                }
                case MPV_EVENT_COMMAND_REPLY:
                    if (event->error < 0) {
                        this->has_media = false;
                        this->session_active.store(false);
                        this->last_error = std::string("mpv command failed: ") + mpv_error_string(event->error);
                        append_debug_log("[event] MPV_EVENT_COMMAND_REPLY error: " + this->last_error);
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
                        this->last_error = message->text;
                        append_debug_log(std::string("[event] ffmpeg error: ") + this->last_error);
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
        mpv_set_option_string(this->handle, "hwdec", "auto");
        mpv_set_option_string(this->handle, "pause", "no");
        mpv_set_option_string(this->handle, "audio-display", "no");

        if (mpv_initialize(this->handle) < 0) {
            error_message = "mpv_initialize failed.";
            return false;
        }

        mpv_observe_property(this->handle, 0, "pause", MPV_FORMAT_FLAG);

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_create(&this->context, this->handle, params) < 0) {
            error_message = "mpv_render_context_create failed.";
            return false;
        }

        mpv_render_context_set_update_callback(this->context, &SwitchMpvPlayer::mpv_render_update, this);
        this->ready = true;
        return true;
    }

    void ensure_buffer(int width, int height) {
        if (width == this->buffer_width && height == this->buffer_height) {
            return;
        }

        this->buffer_width = width;
        this->buffer_height = height;
        this->stride = static_cast<size_t>(this->buffer_width) * 4;
        this->pixels.assign(this->stride * static_cast<size_t>(this->buffer_height), 0);
        this->frame_dirty.store(true);
    }

    void render_frame() {
        if (this->context == nullptr || this->pixels.empty()) {
            return;
        }

        int size[] = {this->buffer_width, this->buffer_height};
        const char* format = "rgba";
        int stride_value = static_cast<int>(this->stride);
        void* pixel_pointer = this->pixels.data();

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_SW_SIZE, size},
            {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>(format)},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride_value},
            {MPV_RENDER_PARAM_SW_POINTER, pixel_pointer},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        mpv_render_context_render(this->context, params);
    }

    SwitchMpvPlayer() = default;

    ~SwitchMpvPlayer() {
        if (this->context != nullptr) {
            mpv_render_context_free(this->context);
            this->context = nullptr;
        }

        if (this->handle != nullptr) {
            mpv_terminate_destroy(this->handle);
            this->handle = nullptr;
        }
    }

    mpv_handle* handle = nullptr;
    mpv_render_context* context = nullptr;
    bool ready = false;
    bool has_media = false;
    std::atomic<bool> session_active = false;
    std::atomic<bool> paused = false;
    std::atomic<bool> frame_dirty = false;
    double playback_speed = 1.0;
    int playback_volume = 80;
    std::string last_error;
    std::filesystem::path cache_file_path;

    int buffer_width = 0;
    int buffer_height = 0;
    size_t stride = 0;
    std::vector<std::uint8_t> pixels;
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

bool switch_mpv_set_volume(int volume) {
    return SwitchMpvPlayer::instance().set_volume(volume);
}

int switch_mpv_get_volume() {
    return SwitchMpvPlayer::instance().get_volume();
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

bool switch_mpv_render_rgba_frame(
    int width,
    int height,
    const std::uint8_t** rgba_data,
    size_t* rgba_size,
    int* rgba_stride) {
    return SwitchMpvPlayer::instance().render_rgba_frame(
        std::max(1, width),
        std::max(1, height),
        rgba_data,
        rgba_size,
        rgba_stride);
}

#else

bool switch_mpv_backend_available() {
    return false;
}

std::string switch_mpv_backend_reason() {
    return "switch-libmpv is not installed in devkitPro portlibs.";
}

bool switch_mpv_open(const PlaybackTarget&, std::string& error_message) {
    error_message = switch_mpv_backend_reason();
    return false;
}

void switch_mpv_stop() {
}

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

bool switch_mpv_set_volume(int) {
    return false;
}

int switch_mpv_get_volume() {
    return 100;
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

bool switch_mpv_render_rgba_frame(
    int,
    int,
    const std::uint8_t**,
    size_t*,
    int*) {
    return false;
}

#endif

}  // namespace switchbox::core
