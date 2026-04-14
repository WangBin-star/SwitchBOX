#pragma once

#include <string>

#include <borealis.hpp>

namespace switchbox::app {

class PlayerVideoSurface : public brls::Box {
public:
    PlayerVideoSurface();
    ~PlayerVideoSurface() override;

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
};

}  // namespace switchbox::app
