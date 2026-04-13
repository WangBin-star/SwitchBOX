#include <filesystem>

#include <windows.h>

#include "switchbox/app/application.hpp"
#include "switchbox/app/startup_context.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/platform/desktop/platform_desktop.hpp"

namespace {

void align_working_directory_with_resources() {
    std::wstring modulePath(MAX_PATH, L'\0');
    const DWORD pathLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));

    if (pathLength == 0 || pathLength == modulePath.size()) {
        return;
    }

    modulePath.resize(pathLength);

    const std::filesystem::path executablePath(modulePath);
    const std::filesystem::path executableDir = executablePath.parent_path();
    const std::filesystem::path buildDir = executableDir.parent_path();

    const std::filesystem::path siblingResources = executableDir / "resources" / "font" / "switch_font.ttf";
    if (std::filesystem::exists(siblingResources)) {
        std::filesystem::current_path(executableDir);
        return;
    }

    const std::filesystem::path parentResources = buildDir / "resources" / "font" / "switch_font.ttf";
    if (std::filesystem::exists(parentResources)) {
        std::filesystem::current_path(buildDir);
    }
}

}  // namespace

int main() {
    align_working_directory_with_resources();

    switchbox::app::StartupContext context{
        .platform_name = switchbox::platform::desktop::platform_name(),
        .switch_target = false,
        .debug_host = true,
    };

    switchbox::app::Application application;
    return application.run(context);
}
