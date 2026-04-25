#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "switchbox/app/player_video_surface.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/iptv_playlist.hpp"
#include "switchbox/core/smb_browser.hpp"
#include "switchbox/core/playback_target.hpp"

namespace switchbox::app {

class PlayerActivity : public brls::Activity {
public:
    explicit PlayerActivity(switchbox::core::PlaybackTarget target);
    ~PlayerActivity() override;
    void willAppear(bool resetState = false) override;

private:
    void initialize_switch_player_state();
    void start_playback_with_target(const switchbox::core::PlaybackTarget& next_target);
    bool prepare_switch_renderer_if_needed(std::string& error_message);
    void save_player_volume_if_needed();
    void begin_async_startup_for_target(const switchbox::core::PlaybackTarget& next_target);
    void poll_pending_startup_task();
    void cancel_pending_startup_task();
    void apply_started_target_state(const switchbox::core::PlaybackTarget& next_target);
    void update_startup_loading_overlay_state();
    void queue_startup_dialog(
        std::string message,
        bool return_to_previous_activity,
        bool stop_playback_before_dialog = false);
    void present_startup_dialog_if_needed();
    void dismiss_to_previous_activity_if_still_top();

    bool handle_a_action();
    bool handle_b_action();
    bool handle_x_action();
    bool handle_y_press();
    bool handle_plus_action();
    bool handle_left_action();
    bool handle_right_action();
    bool handle_up_action();
    bool handle_down_action();
    bool handle_short_backward();
    bool handle_short_forward();
    bool handle_long_backward();
    bool handle_long_forward();

    void refresh_overlay_entries(bool keep_selection);
    void sync_overlay_to_surface();
    void move_overlay_selection(int delta);
    void enter_overlay_selection();
    void overlay_go_parent();
    void toggle_overlay();
    void toggle_controls_panel();
    void move_controls_selection(int delta);
    bool should_accept_controls_navigation_step(int direction);
    bool execute_controls_action(int action_index);
    void tick_runtime_controls();
    void apply_directional_input_fallback_if_needed();
    void apply_vertical_repeat_if_needed();
    void apply_controls_horizontal_repeat_if_needed();
    void apply_controls_hold_action_if_needed();
    void apply_continuous_seek_if_needed();
    void apply_hold_speed_if_needed();
    void update_playback_history_state();
    void refresh_resume_restore_state_for_target();
    void maybe_apply_resume_restore();
    void flush_playback_resume_state(bool force);
    void update_auto_sleep_state();
    void update_volume_osd_timeout();
    void present_runtime_error_if_needed();
    void stop_playback_session_before_leave(bool persist_resume = true);
    void adjust_volume(int delta);
    bool seek_relative(double seconds);
    bool seek_absolute(double seconds);
    void refresh_track_selector_state();
    bool open_audio_track_selector();
    bool open_subtitle_track_selector();
    void ensure_controls_panel_visible_for_touch();
    void handle_touch_double_tap();
    void handle_touch_horizontal_pan(brls::GestureState state, float delta_ratio);
    void handle_touch_vertical_pan(brls::GestureState state, float delta_ratio);
    void handle_touch_progress_tap(float ratio);
    void confirm_delete_current_file();
    std::string find_next_focus_after_delete() const;
    switchbox::core::SmbSourceSettings make_smb_source_from_target() const;
    switchbox::core::WebDavSourceSettings make_webdav_source_from_target() const;
    std::vector<size_t> current_iptv_overlay_entry_indices() const;
    std::string current_iptv_overlay_group_title() const;

    struct TrackSelectionState {
        int id = -1;
        std::string label;
        bool selected = false;
    };

    struct OverlayEntryState {
        std::string title;
        std::string stable_key;
        bool is_directory = false;
        bool is_current = false;
        std::string smb_relative_path;
        std::string webdav_relative_path;
        size_t iptv_group_index = std::numeric_limits<size_t>::max();
        size_t iptv_entry_index = std::numeric_limits<size_t>::max();
    };

    struct PendingStartupTask {
        std::shared_ptr<std::atomic_bool> cancel_flag = std::make_shared<std::atomic_bool>(false);
        std::atomic<bool> finished = false;
        std::mutex mutex;
        switchbox::core::PlaybackTarget prepared_target;
        std::string startup_error;
        switchbox::core::IptvOpenPlanProgress progress;
        bool use_custom_loading_state = false;
        std::string loading_message;
        std::string loading_detail;
        float loading_progress = 0.0f;
    };

    switchbox::core::SmbSourceSettings smb_source;
    bool has_smb_source = false;
    switchbox::core::WebDavSourceSettings webdav_source;
    bool has_webdav_source = false;
    std::shared_ptr<const switchbox::core::IptvPlaybackOverlayContext> iptv_overlay_context;
    size_t iptv_overlay_group_index = 0;
    bool iptv_overlay_group_picker = false;
    std::string current_relative_path;
    std::string overlay_relative_path;
    std::vector<OverlayEntryState> overlay_entries;
    int overlay_selected_index = -1;
    bool overlay_visible = false;
    std::string overlay_message;
    bool controls_visible = false;
    int controls_selected_index = 3;
    PlayerVideoSurface* video_surface = nullptr;
    bool player_volume_dirty = false;
    int session_volume = 80;
    double applied_speed = 1.0;
    std::vector<TrackSelectionState> audio_track_options;
    std::vector<TrackSelectionState> subtitle_track_options;
    std::string selected_audio_track_label;
    std::string selected_subtitle_track_label;
    bool audio_track_selectable = false;
    bool subtitle_track_selectable = false;
    bool runtime_initialized = false;
    brls::RepeatingTimer runtime_tick;
    std::chrono::steady_clock::time_point last_continuous_seek_time = std::chrono::steady_clock::time_point::min();
    int last_continuous_seek_mode = 0;
    std::chrono::steady_clock::time_point last_controls_repeat_time = std::chrono::steady_clock::time_point::min();
    int last_controls_repeat_action = -1;
    std::chrono::steady_clock::time_point last_controls_nav_time = std::chrono::steady_clock::time_point::min();
    int last_controls_nav_direction = 0;
    std::chrono::steady_clock::time_point last_controls_horizontal_repeat_time =
        std::chrono::steady_clock::time_point::min();
    int last_controls_horizontal_repeat_direction = 0;
    std::chrono::steady_clock::time_point last_vertical_repeat_time = std::chrono::steady_clock::time_point::min();
    int last_vertical_repeat_direction = 0;
    int button_long_press_threshold_ms = 260;
    bool sticky_speed_enabled = false;
    bool y_button_down = false;
    bool y_hold_speed_active = false;
    std::chrono::steady_clock::time_point y_button_pressed_at = std::chrono::steady_clock::time_point::min();
    bool volume_osd_visible = false;
    std::chrono::steady_clock::time_point volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    bool playback_error_dialog_open = false;
    bool startup_dialog_pending = false;
    bool startup_dialog_returns_to_previous = false;
    bool startup_dialog_stops_playback_before_open = false;
    std::string startup_dialog_message;
    std::shared_ptr<PendingStartupTask> pending_startup_task;
    bool startup_loading_active = false;
    bool startup_loading_overlay_visible = false;
    std::string startup_loading_message;
    std::string startup_loading_detail;
    float startup_loading_progress = 0.0f;
    std::chrono::steady_clock::time_point startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    bool playback_session_stopped = false;
    bool playback_history_recorded_for_target = false;
    std::uint64_t playback_history_restart_at_ms = 0;
    bool resume_restore_pending = false;
    double resume_restore_position_seconds = 0.0;
    std::chrono::steady_clock::time_point resume_restore_started_at = std::chrono::steady_clock::time_point::min();
    std::chrono::steady_clock::time_point last_resume_persist_time = std::chrono::steady_clock::time_point::min();
    bool media_playback_state_enabled = false;
    bool auto_sleep_disabled = false;
    bool touch_horizontal_pan_active = false;
    double touch_seek_anchor_seconds = 0.0;
    bool touch_vertical_pan_active = false;
    int touch_volume_anchor = 0;
    bool dpad_left_stick_up_pressed = false;
    bool dpad_left_stick_down_pressed = false;
    bool dpad_left_stick_left_pressed = false;
    bool dpad_left_stick_right_pressed = false;

    switchbox::core::PlaybackTarget target;
};

}  // namespace switchbox::app
