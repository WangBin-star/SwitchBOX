#pragma once

#include <string>

namespace switchbox::core {

class BuildInfo {
public:
    static std::string app_name();
    static std::string version_string();
};

}  // namespace switchbox::core
