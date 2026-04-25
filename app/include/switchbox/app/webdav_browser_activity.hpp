#pragma once

#include <string>
#include <vector>

#include <borealis.hpp>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/webdav_browser.hpp"

namespace switchbox::app {

class WebDavBrowserActivity : public brls::Activity {
public:
    explicit WebDavBrowserActivity(
        switchbox::core::WebDavSourceSettings source,
        std::string relative_path = {});
    WebDavBrowserActivity(
        switchbox::core::WebDavSourceSettings source,
        switchbox::core::WebDavBrowserResult preloaded_result);
    ~WebDavBrowserActivity() override;
    brls::View* createContentView() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

private:
    void install_common_actions();
    void bind_entry_focus_tracking();
    void sync_focused_entry_from_ui();
    void return_to_home();

    switchbox::core::WebDavSourceSettings source;
    std::string relative_path;
    switchbox::core::WebDavBrowserResult preloaded_result;
    bool has_preloaded_result = false;
    std::vector<switchbox::core::WebDavBrowserEntry> cached_entries;
    bool has_cached_entries = false;
    std::string focused_entry_relative_path;
    bool focused_entry_is_directory = false;
    brls::ActionIdentifier action_back_id = ACTION_NONE;
    brls::ActionIdentifier action_home_id = ACTION_NONE;
};

}  // namespace switchbox::app
