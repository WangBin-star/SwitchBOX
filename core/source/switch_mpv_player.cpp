#include "switchbox/core/switch_mpv_player.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)
#include <mpv/client.h>
#include <mpv/render.h>
#endif

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/smb2_mount_fs.hpp"

namespace switchbox::core {

#if defined(__SWITCH__) && defined(SWITCHBOX_HAS_SWITCH_MPV)

namespace {

void append_debug_log(const std::string& message) {
    (void)message;
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
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

class SwitchMpvPlayer {
public:
    static SwitchMpvPlayer& instance() {
        static SwitchMpvPlayer player;
        return player;
    }

    bool open(const PlaybackTarget& target, std::string& error_message) {
        std::string locator;
        if (!initialize(error_message)) {
            return false;
        }

        if (!stop_current_playback(false)) {
            error_message = "Previous playback session did not stop cleanly.";
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

        append_debug_log("[open] locator=" + locator);

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
        (void)stop_current_playback(false);
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
        this->video_rotation_degrees = (this->video_rotation_degrees + 90) % 360;
        return true;
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

    std::vector<MpvTrackOption> list_tracks_by_type(const char* track_type) {
        std::vector<MpvTrackOption> options;
        if (this->handle == nullptr || track_type == nullptr) {
            return options;
        }

        mpv_node root {};
        const int rc = mpv_get_property(this->handle, "track-list", MPV_FORMAT_NODE, &root);
        if (rc < 0) {
            return options;
        }

        if (root.format == MPV_FORMAT_NODE_ARRAY && root.u.list != nullptr) {
            const mpv_node_list* array = root.u.list;
            for (int index = 0; index < array->num; ++index) {
                const mpv_node& item = array->values[index];
                if (item.format != MPV_FORMAT_NODE_MAP || item.u.list == nullptr) {
                    continue;
                }

                const mpv_node_list* map = item.u.list;
                const std::string type = node_to_string(node_map_find(map, "type"));
                if (type != track_type) {
                    continue;
                }

                int id = -1;
                if (!node_to_int(node_map_find(map, "id"), id)) {
                    continue;
                }

                options.push_back({
                    .id = id,
                    .label = build_track_label(map, id),
                    .selected = node_to_bool(node_map_find(map, "selected")),
                });
            }
        }

        mpv_free_node_contents(&root);
        return options;
    }

    std::vector<MpvTrackOption> list_audio_tracks() {
        return list_tracks_by_type("audio");
    }

    std::vector<MpvTrackOption> list_subtitle_tracks() {
        return list_tracks_by_type("sub");
    }

    bool set_audio_track(int id, std::string& error_message) {
        if (this->handle == nullptr) {
            error_message = "Player backend is not ready.";
            return false;
        }

        int64_t value = static_cast<int64_t>(id);
        const int rc = mpv_set_property(this->handle, "aid", MPV_FORMAT_INT64, &value);
        if (rc < 0) {
            error_message = mpv_error_string(rc);
            return false;
        }

        process_pending_events();
        return true;
    }

    bool set_subtitle_track(int id, std::string& error_message) {
        if (this->handle == nullptr) {
            error_message = "Player backend is not ready.";
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
            error_message = mpv_error_string(rc);
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
    static void mpv_render_update(void* context) {
        auto* self = static_cast<SwitchMpvPlayer*>(context);
        if (self != nullptr) {
            self->frame_dirty.store(true);
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

    bool stop_current_playback(bool release_mount_after_stop) {
        bool fully_stopped = true;
        if (this->handle != nullptr) {
            mpv_command_string(this->handle, "stop");
            fully_stopped = wait_for_stop_completion(std::chrono::milliseconds(1500));
        }

        this->session_active.store(false);
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(false);

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
    int video_rotation_degrees = 0;
    int playback_volume = 80;
    std::string last_error;

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
