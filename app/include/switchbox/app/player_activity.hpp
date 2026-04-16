#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "switchbox/app/player_video_surface.hpp"
#include "switchbox/core/app_config.hpp"
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
    void update_volume_osd_timeout();
    void present_runtime_error_if_needed();
    void stop_playback_session_before_leave();
    void adjust_volume(int delta);
    bool seek_relative(double seconds);
    bool seek_absolute(double seconds);
    void refresh_track_selector_state();
    bool open_audio_track_selector();
    bool open_subtitle_track_selector();
    void ensure_controls_panel_visible_for_touch();
    void handle_touch_horizontal_pan(brls::GestureState state, float delta_ratio);
    void handle_touch_vertical_pan(brls::GestureState state, float delta_ratio);
    void handle_touch_progress_tap(float ratio);
    void confirm_delete_current_file();
    std::string find_next_focus_after_delete() const;
    switchbox::core::SmbSourceSettings make_smb_source_from_target() const;

    struct TrackSelectionState {
        int id = -1;
        std::string label;
        bool selected = false;
    };

    switchbox::core::SmbSourceSettings smb_source;
    bool has_smb_source = false;
    std::string current_relative_path;
    std::string overlay_relative_path;
    std::vector<switchbox::core::SmbBrowserEntry> overlay_entries;
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
    bool volume_osd_visible = false;
    std::chrono::steady_clock::time_point volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    bool playback_error_dialog_open = false;
    bool playback_session_stopped = false;
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
