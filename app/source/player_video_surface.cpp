#include "switchbox/app/player_video_surface.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "switchbox/core/switch_mpv_player.hpp"

namespace switchbox::app {

PlayerVideoSurface::PlayerVideoSurface()
    : brls::Box(brls::Axis::COLUMN) {
    setFocusable(true);
    setHideHighlightBackground(true);
    setGrow(1.0f);
}

PlayerVideoSurface::~PlayerVideoSurface() = default;

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

    if (session_active || has_media || has_frame) {
        this->invalidate();
    }

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace switchbox::app
