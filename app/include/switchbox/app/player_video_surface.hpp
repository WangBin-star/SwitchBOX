#pragma once

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
    int controls_selected_index = 2;
    int short_seek_seconds = 10;
    int long_seek_seconds = 30;
};

class PlayerVideoSurface : public brls::Box {
public:
    PlayerVideoSurface();
    ~PlayerVideoSurface() override;

    void set_overlay_model(PlayerOverlayViewModel model);

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override;

private:
    int image = 0;
    int image_width = 0;
    int image_height = 0;
    std::string last_error;
    PlayerOverlayViewModel overlay_model;
};

}  // namespace switchbox::app
