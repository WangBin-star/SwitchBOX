#include "switchbox/core/switch_mpv_player.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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

constexpr const char* kHwdecCodecs =
    "mpeg1video,mpeg2video,mpeg4,vc1,wmv3,h264,hevc,vp8,vp9,mjpeg";
constexpr int kMpvImageCount = 3;
constexpr unsigned kPresentCmdBufSlices = 3;
constexpr uint32_t kPresentCmdBufSliceSize = 0x10000;

uint32_t align_up_u32(uint64_t value, uint32_t alignment) {
    const uint64_t aligned = (value + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1);
    return static_cast<uint32_t>(aligned);
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
        if (!initialize(error_message)) {
            return false;
        }

        if (this->session_active.load() || this->has_media || !is_idle_active()) {
            if (!stop_current_playback(false)) {
                error_message = "Previous playback session did not stop cleanly.";
                return false;
            }
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
    }

    void shutdown() {
        stop_render_thread();

        if (this->deko3d_queue != nullptr) {
            dkQueueWaitIdle(this->deko3d_queue);
        }

        destroy_render_buffers();
        this->deko3d_device = nullptr;
        this->deko3d_queue = nullptr;

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
        this->last_error.clear();
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

    bool render_deko3d_frame(
        DkDevice device,
        DkQueue queue,
        DkImage* texture,
        int width,
        int height) {
        process_pending_events();

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
        set_property_string_relaxed(
            "alang",
            general.use_preferred_audio_language ? general.preferred_audio_language : "");
        set_property_string_relaxed(
            "slang",
            general.use_preferred_subtitle_language ? general.preferred_subtitle_language : "");
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
            mpv_command_string(this->handle, "stop");
            fully_stopped = wait_for_stop_completion(std::chrono::milliseconds(1500));
        }

        this->session_active.store(false);
        this->has_media = false;
        this->paused.store(false);
        this->frame_dirty.store(false);
        this->current_render_slot.store(-1);

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
                    request_render_update();
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
