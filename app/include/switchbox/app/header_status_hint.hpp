#pragma once

#include <string>

#include <borealis.hpp>

namespace switchbox::app {

class HeaderStatusHint : public brls::Box {
public:
    HeaderStatusHint();

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;

private:
    void update_time();
    void update_wifi();
    void refresh_wifi_icon();

    brls::Label* timeLabel = nullptr;
    brls::Image* wifiImage = nullptr;

    std::string lastTimeText;
    brls::Time lastWifiPoll = 0;
    brls::ThemeVariant appliedTheme = brls::ThemeVariant::LIGHT;
    bool wifiConnected = false;
};

}  // namespace switchbox::app
