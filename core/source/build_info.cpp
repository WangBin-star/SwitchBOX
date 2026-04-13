#include "switchbox/core/build_info.hpp"

namespace switchbox::core {

std::string BuildInfo::app_name() {
    return "SwitchBOX";
}

std::string BuildInfo::version_string() {
    return "0.1.0";
}

}  // namespace switchbox::core
