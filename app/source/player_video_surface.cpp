#include "switchbox/app/player_video_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <string>
#include <utility>

#include <borealis/core/i18n.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/touch/tap_gesture.hpp>

#if defined(__SWITCH__)
#include <borealis/platforms/switch/switch_video.hpp>
#endif

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

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

double monotonic_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string current_time_hhmm_text() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time {};
    localtime_r(&now, &local_time);

    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
    return buffer;
}

std::string current_battery_text() {
    auto* platform = brls::Application::getPlatform();
    if (platform == nullptr || !platform->canShowBatteryLevel()) {
        return "--%";
    }

    const int level = std::clamp(platform->getBatteryLevel(), 0, 100);
    const bool charging = platform->isBatteryCharging();
    return (charging ? "+" : "") + std::to_string(level) + "%";
}

void draw_clipped_single_line_text(
    NVGcontext* vg,
    const std::string& text,
    float area_x,
    float area_y,
    float area_width,
    float area_height,
    bool enable_marquee,
    double marquee_elapsed_seconds) {
    constexpr float kMarqueeGap = 38.0f;
    constexpr float kMarqueeSpeed = 48.0f;
    const float text_y = area_y + area_height * 0.5f;
    float bounds[4] = {};
    const float text_width = nvgTextBounds(vg, 0.0f, 0.0f, text.c_str(), nullptr, bounds);

    nvgSave(vg);
    nvgIntersectScissor(vg, area_x, area_y, area_width, area_height);

    if (!enable_marquee || text_width <= area_width - 2.0f) {
        nvgText(vg, area_x, text_y, text.c_str(), nullptr);
    } else {
        const float cycle = text_width + kMarqueeGap;
        const float elapsed = std::max(0.0f, static_cast<float>(marquee_elapsed_seconds));
        const float offset = std::fmod(elapsed * kMarqueeSpeed, cycle);
        nvgText(vg, area_x - offset, text_y, text.c_str(), nullptr);
        nvgText(vg, area_x - offset + cycle, text_y, text.c_str(), nullptr);
    }

    nvgRestore(vg);
}

void draw_folder_icon(
    NVGcontext* vg,
    float x,
    float center_y,
    float size,
    bool selected) {
    const float icon_width = size;
    const float icon_height = size * 0.72f;
    const float icon_y = center_y - icon_height * 0.5f;
    const float tab_width = icon_width * 0.42f;
    const float tab_height = icon_height * 0.30f;
    const float body_y = icon_y + tab_height * 0.55f;
    const float body_height = icon_height - tab_height * 0.55f;

    const NVGcolor fill_color = selected ? nvgRGBA(255, 215, 120, 245) : nvgRGBA(245, 198, 95, 220);
    const NVGcolor stroke_color = selected ? nvgRGBA(255, 245, 220, 235) : nvgRGBA(235, 225, 195, 205);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, body_y, icon_width, body_height, 2.4f);
    nvgFillColor(vg, fill_color);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 1.4f, icon_y, tab_width, tab_height, 1.8f);
    nvgFillColor(vg, fill_color);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, body_y, icon_width, body_height, 2.4f);
    nvgStrokeColor(vg, stroke_color);
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);
}

void draw_play_or_pause_icon(
    NVGcontext* vg,
    float center_x,
    float center_y,
    float size,
    bool paused_now,
    NVGcolor color) {
    nvgFillColor(vg, color);
    if (paused_now) {
        const float half_h = size * 0.50f;
        const float left = center_x - size * 0.33f;
        const float top = center_y - half_h;
        const float side = half_h * 2.0f;
        const float equilateral_height = side * 0.8660254f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, left, top);
        nvgLineTo(vg, left, top + side);
        nvgLineTo(vg, left + equilateral_height, center_y);
        nvgClosePath(vg);
        nvgFill(vg);
    } else {
        const float bar_h = size * 0.96f;
        const float bar_w = size * 0.24f;
        const float gap = size * 0.16f;
        const float top = center_y - bar_h * 0.5f;
        const float left_bar_x = center_x - gap * 0.5f - bar_w;
        const float right_bar_x = center_x + gap * 0.5f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, left_bar_x, top, bar_w, bar_h, bar_w * 0.28f);
        nvgRoundedRect(vg, right_bar_x, top, bar_w, bar_h, bar_w * 0.28f);
        nvgFill(vg);
    }
}

void draw_rotate_icon(
    NVGcontext* vg,
    float center_x,
    float center_y,
    float size,
    NVGcolor color) {
    const float radius = size * 0.34f;
    const float stroke_width = std::max(2.2f, size * 0.10f);
    const float start_angle = -2.35f;
    const float end_angle = 1.15f;

    nvgBeginPath(vg);
    nvgArc(vg, center_x, center_y, radius, start_angle, end_angle, NVG_CW);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, stroke_width);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);

    const float arrow_tip_x = center_x + std::cos(end_angle) * radius;
    const float arrow_tip_y = center_y + std::sin(end_angle) * radius;
    const float tangent_angle = end_angle + 1.5707963f;
    const float head_length = size * 0.18f;
    const float head_width = size * 0.13f;

    nvgBeginPath(vg);
    nvgMoveTo(vg, arrow_tip_x, arrow_tip_y);
    nvgLineTo(
        vg,
        arrow_tip_x - std::cos(tangent_angle) * head_length - std::cos(end_angle) * head_width,
        arrow_tip_y - std::sin(tangent_angle) * head_length - std::sin(end_angle) * head_width);
    nvgLineTo(
        vg,
        arrow_tip_x - std::cos(tangent_angle) * head_length + std::cos(end_angle) * head_width,
        arrow_tip_y - std::sin(tangent_angle) * head_length + std::sin(end_angle) * head_width);
    nvgClosePath(vg);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

}  // namespace

PlayerVideoSurface::PlayerVideoSurface()
    : brls::Box(brls::Axis::COLUMN) {
    setFocusable(true);
    setHideHighlightBackground(true);
    setGrow(1.0f);

    addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound* sound_to_play) {
        if (status.state != brls::GestureState::END) {
            return;
        }
        if (!this->pause_icon_visible || !this->overlay_model.touch_enable) {
            return;
        }

        const float px = status.position.x;
        const float py = status.position.y;
        const bool inside =
            px >= this->pause_icon_bounds.x &&
            px <= this->pause_icon_bounds.x + this->pause_icon_bounds.width &&
            py >= this->pause_icon_bounds.y &&
            py <= this->pause_icon_bounds.y + this->pause_icon_bounds.height;
        if (!inside) {
            return;
        }

        if (this->pause_icon_tap_handler) {
            this->pause_icon_tap_handler();
            *sound_to_play = brls::SOUND_CLICK;
        }
    }));
}

PlayerVideoSurface::~PlayerVideoSurface() = default;

void PlayerVideoSurface::set_overlay_model(PlayerOverlayViewModel model) {
    const double now_seconds = monotonic_seconds();
    const bool visibility_changed = model.visible != this->overlay_model.visible;
    bool selection_changed = false;
    if (model.visible != this->overlay_model.visible ||
        model.selected_index != this->overlay_model.selected_index) {
        selection_changed = true;
    } else if (model.visible &&
               model.selected_index >= 0 &&
               model.selected_index < static_cast<int>(model.entries.size()) &&
               model.selected_index < static_cast<int>(this->overlay_model.entries.size())) {
        const auto& next_entry = model.entries[static_cast<size_t>(model.selected_index)];
        const auto& current_entry = this->overlay_model.entries[static_cast<size_t>(model.selected_index)];
        if (next_entry.title != current_entry.title ||
            next_entry.is_directory != current_entry.is_directory ||
            next_entry.is_current != current_entry.is_current) {
            selection_changed = true;
        }
    } else if (model.visible && model.selected_index >= 0) {
        selection_changed = true;
    }

    if (selection_changed) {
        this->overlay_selected_since_seconds = now_seconds;
    }
    if (visibility_changed && model.visible) {
        this->overlay_visible_since_seconds = now_seconds;
    }

    this->overlay_model = std::move(model);
    this->invalidate();
}

void PlayerVideoSurface::set_pause_icon_tap_handler(std::function<void()> handler) {
    this->pause_icon_tap_handler = std::move(handler);
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
    const bool has_media = switchbox::core::switch_mpv_has_media();
    const bool session_active = switchbox::core::switch_mpv_session_active();
    const bool paused = switchbox::core::switch_mpv_is_paused();
    const std::string runtime_error = switchbox::core::switch_mpv_consume_last_error();
    if (!runtime_error.empty()) {
        this->last_error = runtime_error;
    }

    bool rendered_video = false;
#if defined(__SWITCH__)
    auto* switch_video_context = dynamic_cast<brls::SwitchVideoContext*>(
        brls::Application::getPlatform()->getVideoContext());
    if (switch_video_context != nullptr && (session_active || has_media)) {
        if (switchbox::core::switch_mpv_render_deko3d_frame(
                switch_video_context->getDeko3dDevice(),
                switch_video_context->getQueue(),
                switch_video_context->getFramebuffer(),
                draw_width,
                draw_height)) {
            rendered_video = true;
        }
    }
#endif

    if (!rendered_video) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        nvgFill(vg);
    }

    if (!this->last_error.empty()) {
        nvgFontSize(vg, 20.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgTextBox(vg, x + 40.0f, y + 40.0f, width - 80.0f, this->last_error.c_str(), nullptr);
    }

    this->pause_icon_visible = false;
    this->pause_icon_bounds = {};
    if (paused && !this->overlay_model.visible && !this->overlay_model.controls_visible) {
        const float icon_height = std::max(96.0f, height * 0.25f);
        const float icon_width = icon_height * 0.82f;
        const float center_x = x + width * 0.5f;
        const float center_y = y + height * 0.5f;
        const float touch_padding = icon_height * 0.16f;
        const float touch_size = icon_height + touch_padding * 2.0f;
        const float touch_x = center_x - touch_size * 0.5f;
        const float touch_y = center_y - touch_size * 0.5f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, touch_x, touch_y, touch_size, touch_size, touch_size * 0.5f);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 120));
        nvgFill(vg);

        const float triangle_left = center_x - icon_width * 0.34f;
        const float triangle_top = center_y - icon_height * 0.5f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, triangle_left, triangle_top);
        nvgLineTo(vg, triangle_left, triangle_top + icon_height);
        nvgLineTo(vg, triangle_left + icon_width, center_y);
        nvgClosePath(vg);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 240));
        nvgFill(vg);

        this->pause_icon_visible = true;
        this->pause_icon_bounds = {
            .x = touch_x,
            .y = touch_y,
            .width = touch_size,
            .height = touch_size,
        };
    }

    if (this->overlay_model.visible) {
        const float panel_width = width * 0.333333f;
        const float panel_x = x;
        const float panel_y = y;
        const float panel_height = height;
        const double now_seconds = monotonic_seconds();
        const double marquee_delay_seconds =
            static_cast<double>(std::max(0, this->overlay_model.overlay_marquee_delay_ms)) / 1000.0;
        const bool path_marquee_enabled =
            (now_seconds - this->overlay_visible_since_seconds) >= marquee_delay_seconds;
        const double path_marquee_elapsed = path_marquee_enabled
                                                ? (now_seconds - this->overlay_visible_since_seconds - marquee_delay_seconds)
                                                : 0.0;

        nvgBeginPath(vg);
        nvgRect(vg, panel_x, panel_y, panel_width, panel_height);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 150));
        nvgFill(vg);

        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        const float header_margin = 18.0f;
        const float header_height = 30.0f;
        const float footer_height = 50.0f;
        const float footer_top = panel_y + panel_height - footer_height;
        const float list_top = panel_y + 74.0f;
        const float list_bottom = footer_top - 10.0f;
        const float row_height = 46.0f;
        const float row_gap = 8.0f;
        const float row_step = row_height + row_gap;
        const float available_list_height = std::max(0.0f, list_bottom - list_top);
        const int max_rows = std::max(1, static_cast<int>((available_list_height + row_gap) / row_step));
        const float list_height = std::max(0.0f, row_step * static_cast<float>(max_rows) - row_gap);

        nvgFontSize(vg, 21.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        draw_clipped_single_line_text(
            vg,
            this->overlay_model.path,
            panel_x + header_margin,
            panel_y + header_margin,
            panel_width - header_margin * 2,
            header_height,
            path_marquee_enabled,
            path_marquee_elapsed);

        int first_row = 0;
        if (this->overlay_model.selected_index >= max_rows) {
            first_row = this->overlay_model.selected_index - max_rows + 1;
        }

        const float scrollbar_width = 8.0f;
        const float scrollbar_right_padding = 8.0f;
        const float scrollbar_x = panel_x + panel_width - scrollbar_right_padding - scrollbar_width;
        const float row_text_right_padding = 14.0f + scrollbar_width + scrollbar_right_padding;

        const int entry_count = static_cast<int>(this->overlay_model.entries.size());
        for (int index = first_row; index < entry_count && index < first_row + max_rows; ++index) {
            const auto& entry = this->overlay_model.entries[static_cast<size_t>(index)];
            const bool selected = index == this->overlay_model.selected_index;
            const float row_y = list_top + static_cast<float>(index - first_row) * row_step;

            if (selected) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, panel_x + 10.0f, row_y - 1.0f, panel_width - 20.0f, row_height, 8.0f);
                nvgFillColor(vg, nvgRGBA(80, 120, 210, 170));
                nvgFill(vg);
            }

            float content_x = panel_x + 16.0f;
            const float row_center_y = row_y + row_height * 0.5f;

            if (entry.is_current) {
                nvgBeginPath(vg);
                nvgCircle(vg, content_x + 5.0f, row_center_y, 4.4f);
                nvgFillColor(vg, selected ? nvgRGBA(255, 255, 255, 250) : nvgRGBA(205, 205, 205, 210));
                nvgFill(vg);
                content_x += 15.0f;
            }

            if (entry.is_directory) {
                draw_folder_icon(vg, content_x, row_center_y, 24.0f, selected);
                content_x += 30.0f;
            }

            nvgFontSize(vg, 31.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, selected ? nvgRGB(255, 255, 255) : nvgRGB(210, 210, 210));
            const bool marquee_enabled =
                selected &&
                (now_seconds - this->overlay_selected_since_seconds) >= marquee_delay_seconds;
            const double marquee_elapsed = marquee_enabled
                                               ? (now_seconds - this->overlay_selected_since_seconds - marquee_delay_seconds)
                                               : 0.0;
            draw_clipped_single_line_text(
                vg,
                entry.title,
                content_x,
                row_y,
                panel_width - (content_x - panel_x) - row_text_right_padding,
                row_height,
                marquee_enabled,
                marquee_elapsed);
        }

        if (entry_count > max_rows && list_height > 1.0f) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, scrollbar_x, list_top, scrollbar_width, list_height, 4.0f);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 45));
            nvgFill(vg);

            const float ratio = static_cast<float>(max_rows) / static_cast<float>(entry_count);
            const float thumb_height = std::max(22.0f, list_height * ratio);
            const int max_first_row = std::max(0, entry_count - max_rows);
            const float normalized_top =
                max_first_row > 0 ? static_cast<float>(first_row) / static_cast<float>(max_first_row) : 0.0f;
            const float thumb_y = list_top + (list_height - thumb_height) * normalized_top;

            nvgBeginPath(vg);
            nvgRoundedRect(vg, scrollbar_x, thumb_y, scrollbar_width, thumb_height, 4.0f);
            nvgFillColor(vg, nvgRGBA(255, 255, 255, 185));
            nvgFill(vg);
        }

        if (!this->overlay_model.message.empty()) {
            nvgFontSize(vg, 15.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
            nvgFillColor(vg, nvgRGB(255, 190, 190));
            nvgTextBox(
                vg,
                panel_x + 14.0f,
                footer_top - 8.0f,
                panel_width - 24.0f,
                this->overlay_model.message.c_str(),
                nullptr);
        }

        nvgBeginPath(vg);
        nvgRect(vg, panel_x + 10.0f, footer_top, panel_width - 20.0f, 1.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 70));
        nvgFill(vg);

        const std::string time_text = current_time_hhmm_text();
        const std::string battery_text = current_battery_text();

        nvgFontSize(vg, 20.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 220));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, panel_x + 16.0f, footer_top + footer_height * 0.5f, time_text.c_str(), nullptr);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, panel_x + panel_width - 16.0f, footer_top + footer_height * 0.5f, battery_text.c_str(), nullptr);
    }

    if (this->overlay_model.controls_visible) {
        nvgFontFaceId(vg, brls::Application::getDefaultFont());

        const float panel_width = width * 0.84f;
        const float panel_height = 132.0f;
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
        const float progress_y = panel_y + 14.0f;
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
        nvgText(vg, progress_x + progress_width, progress_y + 15.0f, progress_text.c_str(), nullptr);

        const int short_seek = std::max(1, this->overlay_model.short_seek_seconds);
        const int long_seek = std::max(1, this->overlay_model.long_seek_seconds);
        const bool paused_now = switchbox::core::switch_mpv_is_paused();
        const std::string transport_labels[6] = {
            "",
            "-" + std::to_string(long_seek) + "s",
            "-" + std::to_string(short_seek) + "s",
            "",
            "+" + std::to_string(short_seek) + "s",
            "+" + std::to_string(long_seek) + "s",
        };

        const float buttons_y = panel_y + 58.0f;
        const float row_gap = 12.0f;
        float selector_width = std::clamp(progress_width * 0.14f, 124.0f, 172.0f);
        const float volume_group_width = std::clamp(progress_width * 0.23f, 210.0f, 260.0f);
        float buttons_area_width = progress_width - volume_group_width - selector_width * 2.0f - row_gap * 3.0f;
        if (buttons_area_width < 396.0f) {
            const float deficit = 396.0f - buttons_area_width;
            const float reduce_per_selector = deficit * 0.5f;
            selector_width = std::max(108.0f, selector_width - reduce_per_selector);
            buttons_area_width = progress_width - volume_group_width - selector_width * 2.0f - row_gap * 3.0f;
        }
        const float buttons_area_x = progress_x;
        const float audio_selector_x = buttons_area_x + buttons_area_width + row_gap;
        const float subtitle_selector_x = audio_selector_x + selector_width + row_gap;
        const float volume_group_x = subtitle_selector_x + selector_width + row_gap;
        const float button_gap = 10.0f;
        const float button_height = 42.0f;
        const float button_width = (buttons_area_width - button_gap * 5.0f) / 6.0f;
        for (int index = 0; index < 6; index++) {
            const float button_x = buttons_area_x + index * (button_width + button_gap);
            const bool selected = this->overlay_model.controls_selected_index == index;

            nvgBeginPath(vg);
            nvgRoundedRect(vg, button_x, buttons_y, button_width, button_height, 10.0f);
            nvgFillColor(vg, selected ? nvgRGBA(80, 120, 210, 220) : nvgRGBA(255, 255, 255, 38));
            nvgFill(vg);

            if (index == 0) {
                draw_rotate_icon(
                    vg,
                    button_x + button_width * 0.5f,
                    buttons_y + button_height * 0.5f,
                    20.0f,
                    nvgRGBA(255, 255, 255, selected ? 255 : 220));
            } else if (index == 3) {
                draw_play_or_pause_icon(
                    vg,
                    button_x + button_width * 0.5f,
                    buttons_y + button_height * 0.5f,
                    18.0f,
                    paused_now,
                    nvgRGBA(255, 255, 255, selected ? 255 : 220));
            } else {
                nvgFontSize(vg, 17.0f);
                nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                nvgFillColor(vg, nvgRGBA(255, 255, 255, selected ? 255 : 220));
                nvgText(
                    vg,
                    button_x + button_width * 0.5f,
                    buttons_y + button_height * 0.5f,
                    transport_labels[index].c_str(),
                    nullptr);
            }
        }

        struct SelectorView {
            const std::string title;
            const std::string value;
            const bool selectable;
            const int action_index;
            float x;
        };

        const SelectorView selectors[2] = {
            {
                .title = tr("player_page/audio/title"),
                .value = this->overlay_model.audio_track_label,
                .selectable = this->overlay_model.audio_track_selectable,
                .action_index = 6,
                .x = audio_selector_x,
            },
            {
                .title = tr("player_page/subtitle/title"),
                .value = this->overlay_model.subtitle_track_label,
                .selectable = this->overlay_model.subtitle_track_selectable,
                .action_index = 7,
                .x = subtitle_selector_x,
            },
        };

        for (const auto& selector : selectors) {
            const bool selected = this->overlay_model.controls_selected_index == selector.action_index;
            const NVGcolor background =
                selector.selectable
                    ? (selected ? nvgRGBA(80, 120, 210, 220) : nvgRGBA(255, 255, 255, 38))
                    : (selected ? nvgRGBA(85, 85, 85, 210) : nvgRGBA(255, 255, 255, 20));
            const NVGcolor title_color =
                selector.selectable
                    ? nvgRGBA(255, 255, 255, selected ? 230 : 180)
                    : nvgRGBA(170, 170, 170, 170);
            const NVGcolor value_color =
                selector.selectable
                    ? nvgRGBA(255, 255, 255, selected ? 255 : 225)
                    : nvgRGBA(175, 175, 175, 180);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, selector.x, buttons_y, selector_width, button_height, 10.0f);
            nvgFillColor(vg, background);
            nvgFill(vg);

            nvgFontSize(vg, 13.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgFillColor(vg, title_color);
            nvgText(vg, selector.x + 8.0f, buttons_y + 4.0f, selector.title.c_str(), nullptr);

            nvgFontSize(vg, 16.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, value_color);
            draw_clipped_single_line_text(
                vg,
                selector.value,
                selector.x + 8.0f,
                buttons_y + 17.0f,
                selector_width - 16.0f,
                button_height - 19.0f,
                false,
                0.0);
        }

        const int volume = std::clamp(switchbox::core::switch_mpv_get_volume(), 0, 100);
        const float volume_ratio = static_cast<float>(volume) / 100.0f;
        const float volume_bar_height = 10.0f;
        const float volume_row_top = buttons_y + (button_height - volume_bar_height) * 0.5f;
        const float volume_center_y = buttons_y + button_height * 0.5f;
        const float volume_text_x = volume_group_x;
        const float volume_text_width = 100.0f;
        const float volume_bar_x = volume_text_x + volume_text_width + 12.0f;
        const float volume_bar_width = std::max(56.0f, panel_x + panel_width - 26.0f - volume_bar_x);

        const std::string volume_text = tr("player_page/volume_label", std::to_string(volume));
        nvgFontSize(vg, 17.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 215));
        nvgText(vg, volume_text_x, volume_center_y, volume_text.c_str(), nullptr);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, volume_bar_x, volume_row_top, volume_bar_width, volume_bar_height, 5.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 50));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, volume_bar_x, volume_row_top, volume_bar_width * volume_ratio, volume_bar_height, 5.0f);
        nvgFillColor(vg, nvgRGBA(120, 220, 130, 220));
        nvgFill(vg);
    }

    if (this->overlay_model.volume_osd_visible) {
        const int volume = std::clamp(this->overlay_model.volume_osd_value, 0, 100);
        const float volume_ratio = static_cast<float>(volume) / 100.0f;
        const float panel_width = 94.0f;
        const float panel_height = 246.0f;
        const float panel_x = x + width - panel_width - 24.0f;
        const float panel_y = y + (height - panel_height) * 0.5f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, panel_x, panel_y, panel_width, panel_height, 14.0f);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 160));
        nvgFill(vg);

        const float bar_width = 16.0f;
        const float bar_height = 150.0f;
        const float bar_x = panel_x + (panel_width - bar_width) * 0.5f;
        const float bar_y = panel_y + 42.0f;
        const float fill_height = bar_height * volume_ratio;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, bar_x, bar_y, bar_width, bar_height, 7.0f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 50));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(
            vg,
            bar_x,
            bar_y + (bar_height - fill_height),
            bar_width,
            fill_height,
            7.0f);
        nvgFillColor(vg, nvgRGBA(120, 220, 130, 220));
        nvgFill(vg);

        const std::string value_text = std::to_string(volume);
        nvgFontSize(vg, 24.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 220));
        nvgText(vg, panel_x + panel_width * 0.5f, panel_y + panel_height - 30.0f, value_text.c_str(), nullptr);
    }

    if (session_active || has_media || rendered_video) {
        this->invalidate();
    }

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

}  // namespace switchbox::app
