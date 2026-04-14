#pragma once

#include <filesystem>
#include <vector>

#include <borealis.hpp>

namespace switchbox::app {

class ConfigMissingActivity : public brls::Activity {
public:
    explicit ConfigMissingActivity(std::vector<std::filesystem::path> searched_paths);

private:
    std::vector<std::filesystem::path> searched_paths;
};

}  // namespace switchbox::app
