#include "switchbox/app/header_status_hint.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace switchbox::app {

namespace {

std::string wifi_asset_for_state(brls::ThemeVariant theme, bool connected) {
    if (theme == brls::ThemeVariant::DARK) {
        return connected ? "img/sys/wifi_3_dark.png" : "img/sys/wifi_0_dark.png";
    }

    return connected ? "img/sys/wifi_3_light.png" : "img/sys/wifi_0_light.png";
}

}  // namespace

HeaderStatusHint::HeaderStatusHint()
    : brls::Box(brls::Axis::ROW) {
    setDimensions(brls::View::AUTO, brls::View::AUTO);
    setAlignItems(brls::AlignItems::CENTER);
    setJustifyContent(brls::JustifyContent::FLEX_END);

    timeLabel = new brls::Label();
    timeLabel->setSingleLine(true);
    timeLabel->setFontSize(20.0f);
    timeLabel->setLineHeight(1.0f);
    timeLabel->setVerticalAlign(brls::VerticalAlign::CENTER);
    timeLabel->setMinHeight(35);
    timeLabel->setMargins(0, 12, 0, 0);
    addView(timeLabel);

    wifiImage = new brls::Image();
    wifiImage->setScalingType(brls::ImageScalingType::FIT);
    wifiImage->setDimensions(44, 44);
    wifiImage->setTranslationY(-2.0f);
    addView(wifiImage);

    update_time();
    update_wifi();
    refresh_wifi_icon();
}

void HeaderStatusHint::draw(
    NVGcontext* vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext* ctx) {
    update_time();
    update_wifi();
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void HeaderStatusHint::update_time() {
    const auto now = std::chrono::system_clock::now();
    const auto asTimeT = std::chrono::system_clock::to_time_t(now);
    const auto localTime = *std::localtime(&asTimeT);

    std::stringstream stream;
    stream << std::put_time(&localTime, "%H:%M:%S");

    const std::string nextValue = stream.str();
    if (nextValue != lastTimeText) {
        lastTimeText = nextValue;
        timeLabel->setText(lastTimeText);
    }
}

void HeaderStatusHint::update_wifi() {
    const brls::Time now = brls::getCPUTimeUsec();
    if (lastWifiPoll != 0 && (now - lastWifiPoll) < 5000000) {
        return;
    }

    lastWifiPoll = now;

    const auto nextTheme = brls::Application::getPlatform()->getThemeVariant();
    const bool nextConnected = brls::Application::getPlatform()->hasWirelessConnection();

    if (nextTheme == appliedTheme && nextConnected == wifiConnected) {
        return;
    }

    appliedTheme = nextTheme;
    wifiConnected = nextConnected;
    refresh_wifi_icon();
}

void HeaderStatusHint::refresh_wifi_icon() {
    timeLabel->setTextColor(brls::Application::getTheme()["brls/text"]);
    wifiImage->setImageFromRes(wifi_asset_for_state(appliedTheme, wifiConnected));
}

}  // namespace switchbox::app
