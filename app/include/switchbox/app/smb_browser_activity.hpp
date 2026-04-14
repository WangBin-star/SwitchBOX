#pragma once

#include <string>

#include <borealis.hpp>

#include "switchbox/core/app_config.hpp"

namespace switchbox::app {

class SmbBrowserActivity : public brls::Activity {
public:
    explicit SmbBrowserActivity(
        switchbox::core::SmbSourceSettings source,
        std::string relative_path = {});

private:
    switchbox::core::SmbSourceSettings source;
    std::string relative_path;
};

}  // namespace switchbox::app
