#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/iptv_playlist.hpp"

namespace switchbox::app {

class IptvBrowserActivity : public brls::Activity {
public:
    explicit IptvBrowserActivity(switchbox::core::IptvSourceSettings source);
    ~IptvBrowserActivity() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

private:
    struct PlaylistGroup {
        std::string title;
        std::vector<size_t> entry_indices;
        bool favorites = false;
    };

    void install_common_actions();
    void start_loading_if_needed();
    void apply_playlist_result(switchbox::core::IptvPlaylistResult result);
    void show_error_and_return_home(const std::string& message);
    void build_group_model();
    void build_grouped_ui();
    void rebuild_right_panel(
        const std::string& preferred_entry_key = "",
        size_t fallback_index = 0,
        bool request_focus = false);
    void select_group(size_t group_index);
    bool handle_back_action();
    bool is_focus_in_right_panel() const;
    void focus_sidebar();
    std::vector<size_t> current_group_entry_indices() const;
    bool is_favorite_entry(const switchbox::core::IptvPlaylistEntry& entry) const;
    bool persist_favorite_keys(const std::vector<std::string>& favorite_keys);
    bool toggle_favorite_for_entry(
        const switchbox::core::IptvPlaylistEntry& entry,
        size_t fallback_index);
    void return_to_home();

    switchbox::core::IptvSourceSettings source;
    switchbox::core::IptvPlaylistResult playlist_result;
    std::vector<PlaylistGroup> groups;
    size_t selected_group_index = 0;
    bool load_started = false;
    bool grouped_ui_ready = false;
    std::shared_ptr<std::atomic_bool> load_cancelled = std::make_shared<std::atomic_bool>(false);
    brls::Sidebar* sidebar = nullptr;
    brls::Box* right_content_box = nullptr;
    brls::ScrollingFrame* right_scrolling_frame = nullptr;
    brls::ActionIdentifier action_back_id = ACTION_NONE;
    brls::ActionIdentifier action_home_id = ACTION_NONE;
};

}  // namespace switchbox::app
