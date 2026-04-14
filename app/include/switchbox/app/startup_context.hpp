#pragma once

#include <string>

namespace switchbox::app {

struct StartupContext {
    std::string platform_name;
    std::string executable_path;
    bool switch_target = false;
    bool debug_host = false;
};

}  // namespace switchbox::app
