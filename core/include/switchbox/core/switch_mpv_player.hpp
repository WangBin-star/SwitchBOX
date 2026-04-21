#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <deko3d.h>
#endif

#include "switchbox/core/playback_target.hpp"

namespace switchbox::core {

struct MpvTrackOption {
    int id = -1;
    std::string label;
    bool selected = false;
};

bool switch_mpv_backend_available();
std::string switch_mpv_backend_reason();

void switch_mpv_begin_debug_log_session();
void switch_mpv_append_debug_log_note(const std::string& message);
std::string switch_mpv_register_memory_text_stream(
    std::string extension,
    std::string data);
bool switch_mpv_open(const PlaybackTarget& target, std::string& error_message);
void switch_mpv_stop();
void switch_mpv_shutdown();
#if defined(__SWITCH__)
bool switch_mpv_prepare_renderer_for_switch(
    DkDevice device,
    DkQueue queue,
    int width,
    int height,
    std::string& error_message);
#endif
bool switch_mpv_session_active();
bool switch_mpv_has_media();
bool switch_mpv_has_rendered_video_frame();
void switch_mpv_toggle_pause();
bool switch_mpv_seek_relative_seconds(double delta_seconds);
bool switch_mpv_seek_absolute_seconds(double target_seconds);
bool switch_mpv_set_speed(double speed);
double switch_mpv_get_speed();
bool switch_mpv_rotate_clockwise_90();
int switch_mpv_get_video_rotation_degrees();
bool switch_mpv_set_volume(int volume);
int switch_mpv_get_volume();
int64_t switch_mpv_get_transfer_speed_bytes_per_second();
std::uint64_t switch_mpv_get_playback_restart_at_ms();
std::vector<MpvTrackOption> switch_mpv_list_audio_tracks();
std::vector<MpvTrackOption> switch_mpv_list_subtitle_tracks();
bool switch_mpv_set_audio_track(int id, std::string& error_message);
bool switch_mpv_set_subtitle_track(int id, std::string& error_message);
double switch_mpv_get_position_seconds();
double switch_mpv_get_duration_seconds();
bool switch_mpv_is_seekable();
bool switch_mpv_is_paused();
std::string switch_mpv_consume_last_error();

#if defined(__SWITCH__)
bool switch_mpv_render_deko3d_frame(
    DkDevice device,
    DkQueue queue,
    DkImage* texture,
    int width,
    int height);
#endif

void switch_mpv_report_render_swap();

}  // namespace switchbox::core
