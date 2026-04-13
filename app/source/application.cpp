#include "switchbox/app/application.hpp"

#include <cstdlib>

#include <borealis.hpp>

#include "switchbox/core/build_info.hpp"
#include "switchbox/app/home_activity.hpp"

namespace switchbox::app {

int Application::run(const StartupContext& context) const {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow(switchbox::core::BuildInfo::app_name());
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_height", 64.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_padding_top_bottom", 8.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_padding_sides", 26.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_title_font_size", 22.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_title_top_offset", 3.0f);
    brls::Application::setGlobalQuit(false);
    brls::Application::pushActivity(new HomeActivity(context));

    while (brls::Application::mainLoop()) {
    }

    return EXIT_SUCCESS;
}

}  // namespace switchbox::app
