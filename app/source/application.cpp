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
    brls::Application::setGlobalQuit(true);
    brls::Application::pushActivity(new HomeActivity(context));

    while (brls::Application::mainLoop()) {
    }

    return EXIT_SUCCESS;
}

}  // namespace switchbox::app
