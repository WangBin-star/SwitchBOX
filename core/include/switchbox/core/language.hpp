#pragma once

#include <string>
#include <vector>

#include "switchbox/core/app_config.hpp"

namespace switchbox::core {

struct LanguageState {
    std::string configured_language;
    std::string active_language;
    bool using_auto = true;
    std::vector<std::string> available_languages;
};

std::vector<std::string> collect_available_languages(const AppPaths& paths);
LanguageState resolve_language_state(const AppPaths& paths, const AppConfig& config);

}  // namespace switchbox::core
