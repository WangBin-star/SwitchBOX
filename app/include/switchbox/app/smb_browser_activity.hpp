#pragma once

#include <string>
#include <vector>

#include <borealis.hpp>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/smb_browser.hpp"

namespace switchbox::app {

class SmbBrowserActivity : public brls::Activity {
public:
    static void request_focus_after_return(
        const switchbox::core::SmbSourceSettings& source,
        std::string directory_relative_path,
        std::string focus_relative_path,
        std::string deleted_relative_path = {});
    static void request_refresh_after_return(
        const switchbox::core::SmbSourceSettings& source,
        std::string directory_relative_path,
        std::string focus_relative_path = {});
    static void request_delete_after_return(
        const switchbox::core::SmbSourceSettings& source,
        std::string directory_relative_path,
        std::string focus_relative_path,
        std::string deleted_relative_path);

    explicit SmbBrowserActivity(
        switchbox::core::SmbSourceSettings source,
        std::string relative_path = {});
    ~SmbBrowserActivity() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;
    void onResume() override;
    bool apply_pending_return_from_player_if_any();

    void refresh_after_player_delete(
        const std::string& directory_relative_path,
        const std::string& focus_relative_path,
        const std::string& deleted_relative_path = {});

private:
    bool consume_pending_refresh_if_any();
    void sync_focused_entry_from_ui();
    void update_focused_entry_state(const std::string& relative_path);
    void ensure_view_visible(brls::View* focus_target);
    bool restore_focus_now(const std::string& preferred_focus_relative_path);
    void schedule_focus_restore(
        const std::string& preferred_focus_relative_path,
        int attempts_remaining = 4);
    void apply_local_delete_result(
        const std::string& deleted_relative_path,
        const std::string& preferred_focus_relative_path);
    void install_common_actions();
    void bind_entry_focus_tracking();
    void confirm_delete_focused_entry();
    void return_to_home();
    std::string find_next_focus_after_delete(const std::string& deleted_relative_path) const;

    switchbox::core::SmbSourceSettings source;
    std::string relative_path;
    std::vector<switchbox::core::SmbBrowserEntry> cached_entries;
    bool has_cached_entries = false;
    std::string focused_entry_relative_path;
    bool focused_entry_is_directory = false;
    brls::ActionIdentifier action_back_id = ACTION_NONE;
    brls::ActionIdentifier action_delete_id = ACTION_NONE;
    brls::ActionIdentifier action_home_id = ACTION_NONE;
};

}  // namespace switchbox::app
