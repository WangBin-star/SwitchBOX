#include "switchbox/app/application.hpp"

#include <iostream>

#include "switchbox/core/build_info.hpp"

namespace switchbox::app {

namespace {

const char* runtime_mode_label(const StartupContext& context) {
    if (context.switch_target) {
        return "Switch-first app target";
    }

    if (context.debug_host) {
        return "Desktop debug target";
    }

    return "Unknown runtime mode";
}

}  // namespace

int Application::run(const StartupContext& context) const {
    std::cout << switchbox::core::BuildInfo::app_name() << '\n';
    std::cout << "Version: " << switchbox::core::BuildInfo::version_string() << '\n';
    std::cout << "Platform: " << context.platform_name << '\n';
    std::cout << "Mode: " << runtime_mode_label(context) << '\n';
    std::cout << "Bootstrap: application shell initialized." << '\n';
    std::cout << "Next: wire UI shell, playback core, IPTV, then SMB validation." << '\n';
    return 0;
}

}  // namespace switchbox::app
