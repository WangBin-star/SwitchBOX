#include <iostream>

#include "switchbox/app/application.hpp"
#include "switchbox/app/startup_context.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/platform/switch/platform_switch.hpp"

int main() {
    switchbox::app::StartupContext context{
        .platform_name = switchbox::platform::nintendo_switch::platform_name(),
        .switch_target = true,
        .debug_host = false,
    };

    switchbox::app::Application application;
    return application.run(context);
}
