#pragma once

#include <functional>
#include <string>
#include <vector>

#include <borealis.hpp>

namespace switchbox::app {

struct PlayerOverlayEntryView {
    std::string title;
    bool is_directory = false;
    bool is_current = false;
};

struct PlayerOverlayViewModel {
    bool visible = false;
    std::string path;
    std::string message;
    int selected_index = -1;
    std::vector<PlayerOverlayEntryView> entries;
    bool controls_visible = false;
    int controls_selected_index = 3;
    int short_seek_seconds = 10;
    int long_seek_seconds = 30;
    std::string audio_track_label;
    bool audio_track_selectable = false;
    std::string subtitle_track_label;
    bool subtitle_track_selectable = false;
    int overlay_marquee_delay_ms = 500;
    bool touch_enable = true;
    bool volume_osd_visible = false;
    int volume_osd_value = 80;
};

class PlayerVideoSurface : public brls::Box {
public:
    PlayerVideoSurface();
    ~PlayerVideoSurface() override;

    void set_overlay_model(PlayerOverlayViewModel model);
    void set_pause_icon_tap_handler(std::function<void()> handler);

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override;

private:
    struct Rect {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    int image = 0;
    int image_width = 0;
    int image_height = 0;
    std::string last_error;
    PlayerOverlayViewModel overlay_model;
    double overlay_selected_since_seconds = 0.0;
    double overlay_visible_since_seconds = 0.0;
    std::function<void()> pause_icon_tap_handler;
    bool pause_icon_visible = false;
    Rect pause_icon_bounds;
};

}  // namespace switchbox::app
