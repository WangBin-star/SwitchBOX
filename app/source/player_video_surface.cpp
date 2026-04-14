#include "switchbox/app/player_video_surface.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "switchbox/core/switch_mpv_player.hpp"

namespace switchbox::app {

namespace {

std::string format_time_seconds(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;

    char buffer[32] = {};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
    }

    return buffer;
}

}  // namespace

PlayerVideoSurface::PlayerVideoSurface()
    : brls::Box(brls::Axis::COLUMN) {
    setFocusable(true);
    setHideHighlightBackground(true);
    setGrow(1.0f);
}

PlayerVideoSurface::~PlayerVideoSurface() = default;

void PlayerVideoSurface::set_overlay_model(PlayerOverlayViewModel model) {
    this->overlay_model = std::move(model);
    this->invalidate();
}

void PlayerVideoSurface::draw(
    NVGcontext* vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext* ctx) {
    const int draw_width = std::max(1, static_cast<int>(width));
    const int draw_height = std::max(1, static_cast<int>(height));

    const std::uint8_t* rgba_data = nullptr;
    size_t rgba_size = 0;
    int rgba_stride = 0;
    const bool has_frame = switchbox::core::switch_mpv_render_rgba_frame(
        draw_width,
        draw_height,
        &rgba_data,
        &rgba_size,
        &rgba_stride);
    const bool has_media = switchbox::core::switch_mpv_has_media();
    const bool session_active = switchbox::core::switch_mpv_session_active();
    const bool paused = switchbox::core::switch_mpv_is_paused();
    const std::string runtime_error = switchbox::core::switch_mpv_consume_last_error();
    if (!runtime_error.empty()) {
        this->last_error = runtime_error;
    }

    if (has_frame && rgba_data != nullptr && rgba_size > 0 && rgba_stride > 0) {
        if (this->image == 0 || this->image_width != draw_width || this->image_height != draw_height) {
            if (this->image != 0) {
                nvgDeleteImage(vg, this->image);
            }

            this->image = nvgCreateImageRGBA(
                vg,
                draw_width,
                draw_height,
                0,
                reinterpret_cast<const unsigned char*>(rgba_data));
            this->image_width = draw_width;
            this->image_height = draw_height;
        } else {
            nvgUpdateImage(vg, this->image, reinterpret_cast<const unsigned char*>(rgba_data));
        }
    }

    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGB(0, 0, 0));
    nvgFill(vg);

    if (this->image != 0) {
        NVGpaint paint = nvgImagePattern(vg, x, y, width, height, 0.0f, this->image, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

    std::string overlay_status;
    if (!this->last_error.empty()) {
        overlay_status = this->last_error;
    } else if (has_media) {
        overlay_status = paused ? "Paused" : "Playing";
    } else if (session_active) {
        overlay_status = "Loading media...";
    } else {
        overlay_status = "Playback session not active.";
    }

    if (!overlay_status.empty()) {
        nvgFontSize(vg, 20.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextBox(vg, x + 40.0f, y + 40.0f, width - 80.0f, overlay_status.c_str(), nullptr);
    }

    if (this->overlay_model.visible) {
        const float panel_width = width * 0.25f;
        const float panel_x = x;
        const float panel_y = y;
        const float panel_height = height;

        nvgBeginPath(vg);
        nvgRect(vg, panel_x, panel_y, panel_width, panel_height);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 150));
        nvgFill(vg);

        const float header_margin = 18.0f;
        nvgFontSize(vg, 18.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextBox(vg, panel_x + header_margin, panel_y + header_margin, panel_width - header_margin * 2, this->overlay_model.path.c_str(), nullptr);

        float row_y = panel_y + 58.0f;
        const float row_height = 28.0f;
        const int max_rows = std::max(1, static_cast<int>((panel_height - 100.0f) / row_height));
        int first_row = 0;
        if (this->overlay_model.selected_index >= max_rows) {
            first_row = this->overlay_model.selected_index - max_rows + 1;
        }

        const int entry_count = static_cast<int>(this->overlay_model.entries.size());
        for (int index = first_row; index < entry_count && index < first_row + max_rows; ++index) {
            const auto& entry = this->overlay_model.entries[static_cast<size_t>(index)];
            const bool selected = index == this->overlay_model.selected_index;

            if (selected) {
                nvgBeginPath(vg);
                nvgRect(vg, panel_x + 10.0f, row_y - 2.0f, panel_width - 20.0f, row_height);
                nvgFillColor(vg, nvgRGBA(80, 120, 210, 170));
                nvgFill(vg);
            }

            std::string prefix;
            if (entry.is_current) {
                prefix += "▶ ";
            }
            prefix += entry.is_directory ? "[D] " : "[F] ";
            const std::string line = prefix + entry.title;

            nvgFontSize(vg, 17.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, selected ? nvgRGB(255, 255, 255) : nvgRGB(210, 210, 210));
            nvgTextBox(vg, panel_x + 16.0f, row_y, panel_width - 28.0f, line.c_str(), nullptr);
            row_y += row_height;
        }

        if (!this->overlay_model.message.empty()) {
            nvgFontSize(vg, 15.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
            nvgFillColor(vg, nvgRGB(255, 190, 190));
            nvgTextBox(vg, panel_x + 14.0f, panel_y + panel_height - 12.0f, panel_width - 24.0f, this->overlay_model.message.c_str(), nullptr);
        }
    }

    if (this->overlay_model.controls_visible) {
        const float panel_width = width * 0.72f;
        const float panel_height = 190.0f;
        const float panel_x = x + (width - panel_width) * 0.5f;
        const float panel_y = y + height - panel_height - 26.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, panel_x, panel_y, panel_width, panel_height, 18.0f);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
        nvgFill(vg);

        const double duration = switchbox::core::switch_mpv_get_duration_seconds();
        const double position = switchbox::core::switch_mpv_get_position_seconds();
        const float ratio =
            duration > 0.0 ? std::clamp(static_cast<float>(position / duration), 0.0f, 1.0f) : 0.0f;

        const float progress_x = panel_x + 26.0f;
        const float progress_y = panel_y + 24.0f;
        const float progress_width = panel_width - 52.0f;
        const float progress_height = 10.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, progress_x, progress_y, progress_width, progress_height, 5.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 55));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, progress_x, progress_y, progress_width * ratio, progress_height, 5.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 220));
        nvgFill(vg);

        const std::string progress_text =
            format_time_seconds(position) + " / " + format_time_seconds(duration);
        nvgFontSize(vg, 16.0f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 190));
        nvgText(vg, progress_x + progress_width, progress_y + 16.0f, progress_text.c_str(), nullptr);

        const int short_seek = std::max(1, this->overlay_model.short_seek_seconds);
        const int long_seek = std::max(1, this->overlay_model.long_seek_seconds);
        const bool paused_now = switchbox::core::switch_mpv_is_paused();
        const std::string labels[5] = {
            "-" + std::to_string(long_seek) + "s",
            "-" + std::to_string(short_seek) + "s",
            paused_now ? "Play" : "Pause",
            "+" + std::to_string(short_seek) + "s",
            "+" + std::to_string(long_seek) + "s",
        };

        const float buttons_y = panel_y + 70.0f;
        const float button_gap = 10.0f;
        const float button_height = 46.0f;
        const float button_width = (panel_width - 52.0f - button_gap * 4.0f) / 5.0f;
        for (int index = 0; index < 5; ++index) {
            const float button_x = panel_x + 26.0f + index * (button_width + button_gap);
            const bool selected = this->overlay_model.controls_selected_index == index;

            nvgBeginPath(vg);
            nvgRoundedRect(vg, button_x, buttons_y, button_width, button_height, 10.0f);
            nvgFillColor(vg, selected ? nvgRGBA(80, 120, 210, 220) : nvgRGBA(255, 255, 255, 38));
            nvgFill(vg);

            nvgFontSize(vg, 17.0f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, selected ? 255 : 220));
            nvgText(vg, button_x + button_width * 0.5f, buttons_y + button_height * 0.5f, labels[index].c_str(), nullptr);
        }

        const int volume = std::clamp(switchbox::core::switch_mpv_get_volume(), 0, 100);
        const float volume_ratio = static_cast<float>(volume) / 100.0f;
        const float volume_x = panel_x + 26.0f;
        const float volume_y = panel_y + 140.0f;
        const float volume_width = panel_width - 52.0f;
        const float volume_height = 10.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, volume_x, volume_y, volume_width, volume_height, 5.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 50));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, volume_x, volume_y, volume_width * volume_ratio, volume_height, 5.0f);
        nvgFillColor(vg, nvgRGBA(120, 220, 130, 220));
        nvgFill(vg);

        const std::string volume_text = "Volume " + std::to_string(volume);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 205));
        nvgText(vg, volume_x, volume_y + 14.0f, volume_text.c_str(), nullptr);
    }

    if (session_active || has_media || has_frame) {
        this->invalidate();
    }

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace switchbox::app
