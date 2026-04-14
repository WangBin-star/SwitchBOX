#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "switchbox/core/playback_target.hpp"

namespace switchbox::core {

bool switch_mpv_backend_available();
std::string switch_mpv_backend_reason();

bool switch_mpv_open(const PlaybackTarget& target, std::string& error_message);
void switch_mpv_stop();
bool switch_mpv_session_active();
bool switch_mpv_has_media();
void switch_mpv_toggle_pause();
bool switch_mpv_seek_relative_seconds(double delta_seconds);
bool switch_mpv_set_speed(double speed);
double switch_mpv_get_speed();
bool switch_mpv_set_volume(int volume);
int switch_mpv_get_volume();
double switch_mpv_get_position_seconds();
double switch_mpv_get_duration_seconds();
bool switch_mpv_is_paused();
std::string switch_mpv_consume_last_error();

bool switch_mpv_render_rgba_frame(
    int width,
    int height,
    const std::uint8_t** rgba_data,
    size_t* rgba_size,
    int* rgba_stride);

}  // namespace switchbox::core
