#include <iostream>

#include <switch.h>

#include "switchbox/app/application.hpp"
#include "switchbox/app/startup_context.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/platform/switch/platform_switch.hpp"

extern "C" u32 __nx_applet_exit_mode, __nx_nv_service_type, __nx_nv_transfermem_size;

int main(int argc, char* argv[]) {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    const AppletType applet_type = appletGetAppletType();
    const bool application_mode =
        applet_type == AppletType_Application || applet_type == AppletType_SystemApplication;

    __nx_nv_service_type = NvServiceType_Factory;
    __nx_nv_transfermem_size = (application_mode ? 16 : 3) * 0x100000;

    switchbox::app::StartupContext context{
        .platform_name = switchbox::platform::nintendo_switch::platform_name(),
        .executable_path = argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "",
        .switch_target = true,
        .debug_host = false,
    };

    switchbox::app::Application application;
    return application.run(context);
}
