#pragma once

#include <string>

#include <borealis.hpp>

#include "switchbox/core/app_config.hpp"

namespace switchbox::app {

class SmbBrowserActivity : public brls::Activity {
public:
    static void request_focus_after_return(
        const switchbox::core::SmbSourceSettings& source,
        std::string directory_relative_path,
        std::string focus_relative_path);

    explicit SmbBrowserActivity(
        switchbox::core::SmbSourceSettings source,
        std::string relative_path = {});

    void refresh_after_player_delete(
        const std::string& directory_relative_path,
        const std::string& focus_relative_path);

private:
    switchbox::core::SmbSourceSettings source;
    std::string relative_path;
};

}  // namespace switchbox::app
