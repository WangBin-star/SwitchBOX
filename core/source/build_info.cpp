#include "switchbox/core/build_info.hpp"

namespace switchbox::core {

std::string BuildInfo::app_name() {
    return "SwitchBOX";
}

std::string BuildInfo::version_string() {
#ifdef SWITCHBOX_APP_VERSION
    return SWITCHBOX_APP_VERSION;
#else
    return "1.0.1";
#endif
}

}  // namespace switchbox::core
