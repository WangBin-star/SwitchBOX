#include <iostream>

#include "switchbox/app/application.hpp"
#include "switchbox/app/startup_context.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/platform/desktop/platform_desktop.hpp"

int main() {
    switchbox::app::StartupContext context{
        .platform_name = switchbox::platform::desktop::platform_name(),
        .switch_target = false,
        .debug_host = true,
    };

    switchbox::app::Application application;
    return application.run(context);
}
