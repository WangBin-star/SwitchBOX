#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/iptv_playlist.hpp"
#include "switchbox/core/playback_target.hpp"

namespace switchbox::app {

class IptvBrowserDataSource;

bool iptv_debug_log_enabled();
void begin_iptv_debug_log_session();

class IptvBrowserActivity : public brls::Activity {
public:
    explicit IptvBrowserActivity(switchbox::core::IptvSourceSettings source);
    IptvBrowserActivity(
        switchbox::core::IptvSourceSettings source,
        switchbox::core::IptvPlaylistResult preloaded_result);
    ~IptvBrowserActivity() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

private:
    friend class IptvBrowserDataSource;

    struct PlaylistGroup {
        std::string title;
        std::vector<size_t> entry_indices;
        bool favorites = false;
    };

    void install_common_actions();
    void refresh_source_from_config_if_available();
    void start_loading_if_needed();
    void apply_playlist_result(switchbox::core::IptvPlaylistResult result);
    void show_error_and_return_home(const std::string& message);
    void build_group_model();
    void build_grouped_ui();
    void rebuild_right_panel(
        const std::string& preferred_entry_key = "",
        size_t fallback_index = 0,
        bool request_focus = false);
    void show_exit_confirm_dialog();
    void select_group(size_t group_index);
    bool handle_back_action();
    bool is_focus_in_right_panel() const;
    int focused_right_panel_row() const;
    bool move_right_panel_focus(int delta);
    void focus_sidebar();
    std::vector<size_t> current_group_entry_indices() const;
    std::vector<size_t> group_entry_indices_for(size_t group_index) const;
    std::shared_ptr<const switchbox::core::IptvPlaybackOverlayContext> build_playback_overlay_context() const;
    bool is_favorite_entry(const switchbox::core::IptvPlaylistEntry& entry) const;
    bool persist_favorite_keys(const std::vector<std::string>& favorite_keys);
    bool toggle_favorite_for_entry(
        const switchbox::core::IptvPlaylistEntry& entry,
        size_t fallback_index);
    switchbox::core::PlaybackTarget build_playback_target_for_entry(size_t entry_index) const;
    void return_to_previous_activity();
    void return_to_home();

    switchbox::core::IptvSourceSettings source;
    switchbox::core::IptvPlaylistResult playlist_result;
    std::vector<PlaylistGroup> groups;
    size_t selected_group_index = 0;
    bool load_started = false;
    bool grouped_ui_ready = false;
    bool has_preloaded_playlist_result = false;
    switchbox::core::IptvPlaylistResult preloaded_playlist_result;
    std::shared_ptr<std::atomic_bool> load_cancelled = std::make_shared<std::atomic_bool>(false);
    brls::Sidebar* sidebar = nullptr;
    brls::Box* right_panel_root = nullptr;
    brls::Label* right_panel_title_label = nullptr;
    brls::Label* right_panel_count_label = nullptr;
    brls::Label* right_panel_empty_label = nullptr;
    brls::RecyclerFrame* right_recycler_frame = nullptr;
    std::vector<size_t> visible_entry_indices;
    std::unique_ptr<IptvBrowserDataSource> right_panel_data_source;
    brls::ActionIdentifier action_back_id = ACTION_NONE;
    brls::ActionIdentifier action_home_id = ACTION_NONE;
    brls::ActionIdentifier action_nav_up_id = ACTION_NONE;
    brls::ActionIdentifier action_nav_down_id = ACTION_NONE;
    bool exit_confirm_open = false;
};

}  // namespace switchbox::app
