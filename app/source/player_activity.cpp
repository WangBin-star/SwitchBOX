#include "switchbox/app/player_activity.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <borealis/core/application.hpp>
#include <borealis/core/box.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/dropdown.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/hint.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/smb_browser_activity.hpp"
#include "switchbox/core/smb_browser.hpp"
#include "switchbox/core/switch_mpv_player.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::string file_name_from_relative_path(const std::string& relative_path) {
    const auto separator = relative_path.find_last_of("/\\");
    if (separator == std::string::npos) {
        return relative_path;
    }
    if (separator + 1 >= relative_path.size()) {
        return relative_path;
    }
    return relative_path.substr(separator + 1);
}

bool direction_left_pressed(const brls::ControllerState& state) {
    return state.buttons[brls::BUTTON_LEFT] || state.buttons[brls::BUTTON_NAV_LEFT];
}

bool direction_right_pressed(const brls::ControllerState& state) {
    return state.buttons[brls::BUTTON_RIGHT] || state.buttons[brls::BUTTON_NAV_RIGHT];
}

bool left_stick_up_pressed(const brls::ControllerState& state) {
    return state.axes[brls::LEFT_Y] < -0.55f;
}

bool left_stick_down_pressed(const brls::ControllerState& state) {
    return state.axes[brls::LEFT_Y] > 0.55f;
}

bool right_stick_up_pressed(const brls::ControllerState& state) {
    return state.axes[brls::RIGHT_Y] < -0.55f;
}

bool right_stick_down_pressed(const brls::ControllerState& state) {
    return state.axes[brls::RIGHT_Y] > 0.55f;
}

bool left_stick_left_pressed(const brls::ControllerState& state) {
    return state.axes[brls::LEFT_X] < -0.55f;
}

bool left_stick_right_pressed(const brls::ControllerState& state) {
    return state.axes[brls::LEFT_X] > 0.55f;
}

bool right_stick_left_pressed(const brls::ControllerState& state) {
    return state.axes[brls::RIGHT_X] < -0.55f;
}

bool right_stick_right_pressed(const brls::ControllerState& state) {
    return state.axes[brls::RIGHT_X] > 0.55f;
}

brls::View* create_switch_player_content(const switchbox::core::PlaybackTarget&) {
    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(0, 0, 0, 0);
    container->setGrow(1.0f);
    container->setBackgroundColor(nvgRGB(0, 0, 0));

    auto* video_surface = new PlayerVideoSurface();
    video_surface->setId("switchbox/player_surface");
    container->addView(video_surface);

    auto* frame = new brls::AppletFrame(container);
    frame->setHeaderVisibility(brls::Visibility::GONE);
    frame->setFooterVisibility(brls::Visibility::GONE);
    frame->setTitle("");
    return frame;
}

}  // namespace

PlayerActivity::PlayerActivity(switchbox::core::PlaybackTarget target)
    : brls::Activity(create_switch_player_content(target))
    , target(std::move(target)) {
}

void PlayerActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (this->runtime_initialized) {
        return;
    }

    initialize_switch_player_state();

    registerAction(
        tr("player_page/actions/toggle_pause"),
        brls::BUTTON_A,
        [this](brls::View*) { return handle_a_action(); },
        false,
        false,
        brls::SOUND_CLICK);
    registerAction(
        brls::getStr("hints/back"),
        brls::BUTTON_B,
        [this](brls::View*) { return handle_b_action(); },
        false,
        false,
        brls::SOUND_CLICK);

    registerAction(
        tr("actions/delete"),
        brls::BUTTON_X,
        [this](brls::View*) { return handle_x_action(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/settings"),
        brls::BUTTON_Y,
        [this](brls::View*) { return handle_y_press(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/save"),
        brls::BUTTON_START,
        [this](brls::View*) { return handle_plus_action(); },
        true,
        false,
        brls::SOUND_CLICK);

    registerAction(
        tr("actions/disable"),
        brls::BUTTON_LB,
        [this](brls::View*) { return handle_short_backward(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_RB,
        [this](brls::View*) { return handle_short_forward(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_LT,
        [this](brls::View*) { return handle_long_backward(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_RT,
        [this](brls::View*) { return handle_long_forward(); },
        true,
        false,
        brls::SOUND_CLICK);

    registerAction(
        tr("actions/disable"),
        brls::BUTTON_UP,
        [this](brls::View*) { return handle_up_action(); },
        true,
        true,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_DOWN,
        [this](brls::View*) { return handle_down_action(); },
        true,
        true,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_NAV_UP,
        [](brls::View*) { return true; },
        true,
        true,
        brls::SOUND_NONE);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_NAV_DOWN,
        [](brls::View*) { return true; },
        true,
        true,
        brls::SOUND_NONE);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_LEFT,
        [this](brls::View*) { return handle_left_action(); },
        true,
        true,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_RIGHT,
        [this](brls::View*) { return handle_right_action(); },
        true,
        true,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_NAV_LEFT,
        [](brls::View*) { return true; },
        true,
        true,
        brls::SOUND_NONE);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_NAV_RIGHT,
        [](brls::View*) { return true; },
        true,
        true,
        brls::SOUND_NONE);

    this->runtime_tick.setPeriod(16);
    this->runtime_tick.setCallback([this]() {
        this->tick_runtime_controls();
    });
    this->runtime_tick.start();

    this->runtime_initialized = true;
}

PlayerActivity::~PlayerActivity() {
    save_player_volume_if_needed();
    stop_playback_session_before_leave();
}

void PlayerActivity::initialize_switch_player_state() {
    this->video_surface = dynamic_cast<PlayerVideoSurface*>(this->getView("switchbox/player_surface"));
    if (this->video_surface != nullptr) {
        brls::Application::giveFocus(this->video_surface);
        this->video_surface->set_pause_icon_tap_handler([this]() {
            if (!switchbox::core::AppConfigStore::current().general.touch_enable) {
                return;
            }
            if (!switchbox::core::switch_mpv_is_paused()) {
                return;
            }
            switchbox::core::switch_mpv_toggle_pause();
        });
    }

    if (this->target.source_kind == switchbox::core::PlaybackSourceKind::Smb &&
        this->target.smb_locator.has_value()) {
        this->has_smb_source = true;
        this->smb_source = make_smb_source_from_target();
        this->current_relative_path = this->target.smb_locator->relative_path;
        this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->current_relative_path);
    }

    this->session_volume = std::clamp(switchbox::core::AppConfigStore::current().general.player_volume, 0, 100);
    this->applied_speed = 1.0;
    start_playback_with_target(this->target);
    switchbox::core::switch_mpv_set_volume(this->session_volume);
}

void PlayerActivity::start_playback_with_target(const switchbox::core::PlaybackTarget& next_target) {
    std::string startup_error;
    if (!switchbox::core::switch_mpv_open(next_target, startup_error) && !startup_error.empty()) {
        auto* dialog = new brls::Dialog(startup_error);
        dialog->open();
        return;
    }

    this->playback_session_stopped = false;
    this->target = next_target;
    if (this->target.smb_locator.has_value()) {
        this->current_relative_path = this->target.smb_locator->relative_path;
        this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->current_relative_path);
    }

    this->applied_speed = 1.0;
    switchbox::core::switch_mpv_set_speed(1.0);
    switchbox::core::switch_mpv_set_volume(this->session_volume);
    refresh_overlay_entries(true);
}

void PlayerActivity::save_player_volume_if_needed() {
    if (!this->player_volume_dirty) {
        return;
    }

    auto& config = switchbox::core::AppConfigStore::mutable_config();
    config.general.player_volume = std::clamp(this->session_volume, 0, 100);
    (void)switchbox::core::AppConfigStore::save();
    this->player_volume_dirty = false;
}

bool PlayerActivity::handle_a_action() {
    if (this->controls_visible) {
        execute_controls_action(this->controls_selected_index);
        if (this->controls_selected_index == 1 ||
            this->controls_selected_index == 2 ||
            this->controls_selected_index == 4 ||
            this->controls_selected_index == 5) {
            this->last_controls_repeat_action = this->controls_selected_index;
            this->last_controls_repeat_time = std::chrono::steady_clock::now();
        } else {
            this->last_controls_repeat_action = -1;
        }
        return true;
    }

    if (this->overlay_visible) {
        enter_overlay_selection();
        return true;
    }

    switchbox::core::switch_mpv_toggle_pause();
    return true;
}

bool PlayerActivity::handle_b_action() {
    if (this->controls_visible) {
        this->controls_visible = false;
        sync_overlay_to_surface();
        return true;
    }

    if (this->overlay_visible) {
        overlay_go_parent();
        return true;
    }

    stop_playback_session_before_leave();
    brls::Application::popActivity(brls::TransitionAnimation::FADE);
    return true;
}

bool PlayerActivity::handle_x_action() {
    if (this->controls_visible || this->overlay_visible) {
        return true;
    }
    confirm_delete_current_file();
    return true;
}

bool PlayerActivity::handle_y_press() {
    return true;
}

bool PlayerActivity::handle_plus_action() {
    toggle_controls_panel();
    return true;
}

bool PlayerActivity::handle_left_action() {
    if (this->controls_visible) {
        if (should_accept_controls_navigation_step(-1)) {
            move_controls_selection(-1);
        }
        return true;
    }

    const auto& controller = brls::Application::getControllerState();
    if (controller.buttons[brls::BUTTON_LB] ||
        controller.buttons[brls::BUTTON_RB] ||
        controller.buttons[brls::BUTTON_LT] ||
        controller.buttons[brls::BUTTON_RT]) {
        return true;
    }

    toggle_overlay();
    return true;
}

bool PlayerActivity::handle_right_action() {
    if (this->controls_visible) {
        if (should_accept_controls_navigation_step(1)) {
            move_controls_selection(1);
        }
        return true;
    }

    return true;
}

bool PlayerActivity::handle_up_action() {
    if (this->overlay_visible && !this->controls_visible) {
        move_overlay_selection(-1);
    } else {
        adjust_volume(+5);
    }
    return true;
}

bool PlayerActivity::handle_down_action() {
    if (this->overlay_visible && !this->controls_visible) {
        move_overlay_selection(+1);
    } else {
        adjust_volume(-5);
    }
    return true;
}

bool PlayerActivity::handle_short_backward() {
    if (this->controls_visible) {
        return true;
    }

    const auto& controller = brls::Application::getControllerState();
    if (direction_left_pressed(controller) || direction_right_pressed(controller)) {
        return true;
    }

    return seek_relative(-static_cast<double>(switchbox::core::AppConfigStore::current().general.short_seek));
}

bool PlayerActivity::handle_short_forward() {
    if (this->controls_visible) {
        return true;
    }

    const auto& controller = brls::Application::getControllerState();
    if (direction_left_pressed(controller) || direction_right_pressed(controller)) {
        return true;
    }

    return seek_relative(static_cast<double>(switchbox::core::AppConfigStore::current().general.short_seek));
}

bool PlayerActivity::handle_long_backward() {
    if (this->controls_visible) {
        return true;
    }

    return seek_relative(-static_cast<double>(switchbox::core::AppConfigStore::current().general.long_seek));
}

bool PlayerActivity::handle_long_forward() {
    if (this->controls_visible) {
        return true;
    }

    return seek_relative(static_cast<double>(switchbox::core::AppConfigStore::current().general.long_seek));
}

void PlayerActivity::refresh_track_selector_state() {
    this->audio_track_options.clear();
    this->subtitle_track_options.clear();

    const auto audio_tracks = switchbox::core::switch_mpv_list_audio_tracks();
    this->audio_track_options.reserve(audio_tracks.size());
    this->audio_track_selectable = !audio_tracks.empty();
    this->selected_audio_track_label = tr("player_page/audio/no_options");

    for (const auto& track : audio_tracks) {
        this->audio_track_options.push_back({track.id, track.label, track.selected});
        if (track.selected) {
            this->selected_audio_track_label = track.label;
        }
    }
    if (this->audio_track_selectable && this->selected_audio_track_label == tr("player_page/audio/no_options")) {
        this->selected_audio_track_label = this->audio_track_options.front().label;
    }

    const auto subtitle_tracks = switchbox::core::switch_mpv_list_subtitle_tracks();
    this->subtitle_track_selectable = !subtitle_tracks.empty();
    this->selected_subtitle_track_label = tr("player_page/subtitle/no_options");

    if (this->subtitle_track_selectable) {
        bool selected_found = false;
        this->subtitle_track_options.push_back({-1, tr("player_page/subtitle/off"), false});
        for (const auto& track : subtitle_tracks) {
            this->subtitle_track_options.push_back({track.id, track.label, track.selected});
            if (track.selected) {
                selected_found = true;
                this->selected_subtitle_track_label = track.label;
            }
        }

        if (!selected_found) {
            this->subtitle_track_options.front().selected = true;
            this->selected_subtitle_track_label = this->subtitle_track_options.front().label;
        }
    }
}

bool PlayerActivity::open_audio_track_selector() {
    if (!this->audio_track_selectable || this->audio_track_options.empty()) {
        brls::Application::notify(tr("player_page/audio/no_options"));
        return true;
    }

    std::vector<std::string> labels;
    labels.reserve(this->audio_track_options.size());
    int selected_index = 0;
    for (int index = 0; index < static_cast<int>(this->audio_track_options.size()); ++index) {
        labels.push_back(this->audio_track_options[static_cast<size_t>(index)].label);
        if (this->audio_track_options[static_cast<size_t>(index)].selected) {
            selected_index = index;
        }
    }

    const auto options = this->audio_track_options;
    auto* dropdown = new brls::Dropdown(
        tr("player_page/audio/title"),
        labels,
        [](int) {},
        selected_index,
        [this, options](int selection) {
            if (selection < 0 || selection >= static_cast<int>(options.size())) {
                return;
            }

            std::string error_message;
            if (!switchbox::core::switch_mpv_set_audio_track(
                    options[static_cast<size_t>(selection)].id,
                    error_message)) {
                if (error_message.empty()) {
                    error_message = tr("player_page/audio/no_options");
                }
                brls::Application::notify(error_message);
                return;
            }

            this->refresh_track_selector_state();
            this->sync_overlay_to_surface();
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool PlayerActivity::open_subtitle_track_selector() {
    if (!this->subtitle_track_selectable || this->subtitle_track_options.empty()) {
        brls::Application::notify(tr("player_page/subtitle/no_options"));
        return true;
    }

    std::vector<std::string> labels;
    labels.reserve(this->subtitle_track_options.size());
    int selected_index = 0;
    for (int index = 0; index < static_cast<int>(this->subtitle_track_options.size()); ++index) {
        labels.push_back(this->subtitle_track_options[static_cast<size_t>(index)].label);
        if (this->subtitle_track_options[static_cast<size_t>(index)].selected) {
            selected_index = index;
        }
    }

    const auto options = this->subtitle_track_options;
    auto* dropdown = new brls::Dropdown(
        tr("player_page/subtitle/title"),
        labels,
        [](int) {},
        selected_index,
        [this, options](int selection) {
            if (selection < 0 || selection >= static_cast<int>(options.size())) {
                return;
            }

            std::string error_message;
            if (!switchbox::core::switch_mpv_set_subtitle_track(
                    options[static_cast<size_t>(selection)].id,
                    error_message)) {
                if (error_message.empty()) {
                    error_message = tr("player_page/subtitle/no_options");
                }
                brls::Application::notify(error_message);
                return;
            }

            this->refresh_track_selector_state();
            this->sync_overlay_to_surface();
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

void PlayerActivity::refresh_overlay_entries(bool keep_selection) {
    this->overlay_message.clear();
    this->overlay_entries.clear();

    if (!this->has_smb_source) {
        sync_overlay_to_surface();
        return;
    }

    std::string selected_relative_path;
    if (keep_selection &&
        this->overlay_selected_index >= 0 &&
        this->overlay_selected_index < static_cast<int>(this->overlay_entries.size())) {
        selected_relative_path =
            this->overlay_entries[static_cast<size_t>(this->overlay_selected_index)].relative_path;
    }

    const auto& config = switchbox::core::AppConfigStore::current();
    const auto result =
        switchbox::core::browse_smb_directory(
            this->smb_source,
            config.general,
            this->overlay_relative_path);
    if (!result.success) {
        this->overlay_message = result.error_message;
        this->overlay_selected_index = -1;
        sync_overlay_to_surface();
        return;
    }

    this->overlay_entries = result.entries;
    if (this->overlay_entries.empty()) {
        this->overlay_selected_index = -1;
        sync_overlay_to_surface();
        return;
    }

    int matched_index = -1;
    if (!selected_relative_path.empty()) {
        for (int index = 0; index < static_cast<int>(this->overlay_entries.size()); ++index) {
            if (this->overlay_entries[static_cast<size_t>(index)].relative_path == selected_relative_path) {
                matched_index = index;
                break;
            }
        }
    }

    if (matched_index < 0 && !this->current_relative_path.empty()) {
        for (int index = 0; index < static_cast<int>(this->overlay_entries.size()); ++index) {
            if (this->overlay_entries[static_cast<size_t>(index)].relative_path == this->current_relative_path) {
                matched_index = index;
                break;
            }
        }
    }

    this->overlay_selected_index = matched_index >= 0 ? matched_index : 0;
    sync_overlay_to_surface();
}

void PlayerActivity::sync_overlay_to_surface() {
    if (this->video_surface == nullptr) {
        return;
    }

    refresh_track_selector_state();

    PlayerOverlayViewModel model;
    const auto& general = switchbox::core::AppConfigStore::current().general;
    model.visible = this->overlay_visible;
    model.path = this->has_smb_source ? switchbox::core::smb_display_path(this->smb_source, this->overlay_relative_path)
                                      : this->target.source_label;
    model.message = this->overlay_message;
    model.selected_index = this->overlay_selected_index;
    model.controls_visible = this->controls_visible;
    model.controls_selected_index = this->controls_selected_index;
    model.short_seek_seconds = general.short_seek;
    model.long_seek_seconds = general.long_seek;
    model.audio_track_label = this->selected_audio_track_label;
    model.audio_track_selectable = this->audio_track_selectable;
    model.subtitle_track_label = this->selected_subtitle_track_label;
    model.subtitle_track_selectable = this->subtitle_track_selectable;
    model.overlay_marquee_delay_ms = general.overlay_marquee_delay_ms;
    model.touch_enable = general.touch_enable;
    model.volume_osd_visible = this->volume_osd_visible && !this->controls_visible && !this->overlay_visible;
    model.volume_osd_value = std::clamp(this->session_volume, 0, 100);
    model.entries.reserve(this->overlay_entries.size());
    for (const auto& entry : this->overlay_entries) {
        model.entries.push_back({
            .title = entry.name,
            .is_directory = entry.is_directory,
            .is_current = entry.relative_path == this->current_relative_path,
        });
    }

    this->video_surface->set_overlay_model(std::move(model));
}

void PlayerActivity::move_overlay_selection(int delta) {
    if (this->overlay_entries.empty()) {
        return;
    }

    const int count = static_cast<int>(this->overlay_entries.size());
    int next = this->overlay_selected_index;
    if (next < 0) {
        next = 0;
    } else {
        next = (next + delta + count) % count;
    }

    this->overlay_selected_index = next;
    sync_overlay_to_surface();
}

void PlayerActivity::enter_overlay_selection() {
    if (!this->overlay_visible ||
        this->overlay_selected_index < 0 ||
        this->overlay_selected_index >= static_cast<int>(this->overlay_entries.size())) {
        return;
    }

    const auto entry = this->overlay_entries[static_cast<size_t>(this->overlay_selected_index)];
    if (entry.is_directory) {
        this->overlay_relative_path = entry.relative_path;
        refresh_overlay_entries(false);
        return;
    }

    start_playback_with_target(
        switchbox::core::make_smb_playback_target(this->smb_source, entry.relative_path));
}

void PlayerActivity::overlay_go_parent() {
    if (!this->overlay_visible) {
        return;
    }

    if (this->overlay_relative_path.empty()) {
        this->overlay_visible = false;
        sync_overlay_to_surface();
        return;
    }

    this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->overlay_relative_path);
    refresh_overlay_entries(false);
}

void PlayerActivity::toggle_overlay() {
    this->overlay_visible = !this->overlay_visible;
    if (this->overlay_visible) {
        this->volume_osd_visible = false;
        this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    }
    if (this->overlay_visible) {
        refresh_overlay_entries(true);
    } else {
        sync_overlay_to_surface();
    }
}

void PlayerActivity::toggle_controls_panel() {
    this->controls_visible = !this->controls_visible;
    this->last_controls_repeat_action = -1;
    this->last_controls_repeat_time = std::chrono::steady_clock::time_point::min();
    this->last_controls_nav_direction = 0;
    this->last_controls_nav_time = std::chrono::steady_clock::time_point::min();
    if (this->controls_visible) {
        this->volume_osd_visible = false;
        this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    }
    if (this->controls_visible && this->overlay_visible) {
        this->overlay_visible = false;
    }
    sync_overlay_to_surface();
}

bool PlayerActivity::should_accept_controls_navigation_step(int direction) {
    const auto now = std::chrono::steady_clock::now();
    if (this->last_controls_nav_time == std::chrono::steady_clock::time_point::min()) {
        this->last_controls_nav_time = now;
        this->last_controls_nav_direction = direction;
        return true;
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_controls_nav_time).count();
    if (direction == this->last_controls_nav_direction && elapsed_ms < 90) {
        return false;
    }

    this->last_controls_nav_time = now;
    this->last_controls_nav_direction = direction;
    return true;
}

void PlayerActivity::move_controls_selection(int delta) {
    constexpr int kButtonCount = 8;
    int next = this->controls_selected_index;
    if (next < 0 || next >= kButtonCount) {
        next = 3;
    } else {
        next = (next + delta + kButtonCount) % kButtonCount;
    }

    this->controls_selected_index = next;
    sync_overlay_to_surface();
}

bool PlayerActivity::execute_controls_action(int action_index) {
    const auto& general = switchbox::core::AppConfigStore::current().general;
    switch (action_index) {
        case 0:
            return switchbox::core::switch_mpv_rotate_clockwise_90();
        case 1:
            return seek_relative(-static_cast<double>(general.long_seek));
        case 2:
            return seek_relative(-static_cast<double>(general.short_seek));
        case 3:
            switchbox::core::switch_mpv_toggle_pause();
            return true;
        case 4:
            return seek_relative(static_cast<double>(general.short_seek));
        case 5:
            return seek_relative(static_cast<double>(general.long_seek));
        case 6:
            return open_audio_track_selector();
        case 7:
            return open_subtitle_track_selector();
        default:
            return false;
    }
}

void PlayerActivity::tick_runtime_controls() {
    apply_directional_input_fallback_if_needed();
    apply_controls_hold_action_if_needed();
    apply_hold_speed_if_needed();
    apply_continuous_seek_if_needed();
    update_volume_osd_timeout();
    present_runtime_error_if_needed();
}

void PlayerActivity::apply_directional_input_fallback_if_needed() {
    const auto& controller = brls::Application::getControllerState();

    const bool up_pressed =
        left_stick_up_pressed(controller) ||
        right_stick_up_pressed(controller);
    const bool down_pressed =
        left_stick_down_pressed(controller) ||
        right_stick_down_pressed(controller);
    const bool left_pressed =
        left_stick_left_pressed(controller) ||
        right_stick_left_pressed(controller);
    const bool right_pressed =
        left_stick_right_pressed(controller) ||
        right_stick_right_pressed(controller);

    if (up_pressed && !this->dpad_left_stick_up_pressed) {
        handle_up_action();
    }

    if (down_pressed && !this->dpad_left_stick_down_pressed) {
        handle_down_action();
    }

    if (left_pressed && !this->dpad_left_stick_left_pressed) {
        handle_left_action();
    }

    if (right_pressed && !this->dpad_left_stick_right_pressed) {
        handle_right_action();
    }

    this->dpad_left_stick_up_pressed = up_pressed;
    this->dpad_left_stick_down_pressed = down_pressed;
    this->dpad_left_stick_left_pressed = left_pressed;
    this->dpad_left_stick_right_pressed = right_pressed;
}

void PlayerActivity::apply_controls_hold_action_if_needed() {
    if (!this->controls_visible) {
        this->last_controls_repeat_action = -1;
        return;
    }

    const auto& controller = brls::Application::getControllerState();
    if (!controller.buttons[brls::BUTTON_A]) {
        this->last_controls_repeat_action = -1;
        return;
    }

    if (!(this->controls_selected_index == 1 ||
          this->controls_selected_index == 2 ||
          this->controls_selected_index == 4 ||
          this->controls_selected_index == 5)) {
        this->last_controls_repeat_action = -1;
        return;
    }

    const int action = this->controls_selected_index;
    const int interval_ms = std::max(
        10,
        switchbox::core::AppConfigStore::current().general.continuous_seek_interval_ms);
    const auto now = std::chrono::steady_clock::now();

    if (action != this->last_controls_repeat_action) {
        this->last_controls_repeat_action = action;
        this->last_controls_repeat_time = now;
        return;
    }

    if (this->last_controls_repeat_time != std::chrono::steady_clock::time_point::min()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_controls_repeat_time).count();
        if (elapsed < interval_ms) {
            return;
        }
    }

    this->last_controls_repeat_time = now;
    (void)execute_controls_action(action);
}

void PlayerActivity::apply_continuous_seek_if_needed() {
    if (this->overlay_visible || this->controls_visible) {
        this->last_continuous_seek_mode = 0;
        return;
    }

    const auto& controller = brls::Application::getControllerState();
    const bool r_pressed = controller.buttons[brls::BUTTON_RB];
    const bool zr_pressed = controller.buttons[brls::BUTTON_RT];
    const bool left_pressed = direction_left_pressed(controller);
    const bool right_pressed = direction_right_pressed(controller);

    int mode = 0;
    if (zr_pressed && left_pressed) {
        mode = -2;
    } else if (zr_pressed && right_pressed) {
        mode = 2;
    } else if (r_pressed && left_pressed) {
        mode = -1;
    } else if (r_pressed && right_pressed) {
        mode = 1;
    }

    if (mode == 0) {
        this->last_continuous_seek_mode = 0;
        return;
    }

    const int interval_ms = std::max(10, switchbox::core::AppConfigStore::current().general.continuous_seek_interval_ms);
    const auto now = std::chrono::steady_clock::now();
    if (mode != this->last_continuous_seek_mode) {
        this->last_continuous_seek_mode = mode;
        this->last_continuous_seek_time = std::chrono::steady_clock::time_point::min();
    }

    if (this->last_continuous_seek_time != std::chrono::steady_clock::time_point::min()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - this->last_continuous_seek_time).count();
        if (elapsed < interval_ms) {
            return;
        }
    }

    this->last_continuous_seek_time = now;
    const auto& general = switchbox::core::AppConfigStore::current().general;
    const double short_seek = static_cast<double>(general.short_seek);
    const double long_seek = static_cast<double>(general.long_seek);
    switch (mode) {
        case -2:
            seek_relative(-long_seek);
            break;
        case 2:
            seek_relative(long_seek);
            break;
        case -1:
            seek_relative(-short_seek);
            break;
        case 1:
            seek_relative(short_seek);
            break;
        default:
            break;
    }
}

void PlayerActivity::apply_hold_speed_if_needed() {
    const auto& controller = brls::Application::getControllerState();
    const double target_speed =
        controller.buttons[brls::BUTTON_Y]
            ? static_cast<double>(switchbox::core::AppConfigStore::current().general.y_hold_speed_multiplier)
            : 1.0;
    if (std::fabs(target_speed - this->applied_speed) < 0.001) {
        return;
    }

    if (switchbox::core::switch_mpv_set_speed(target_speed)) {
        this->applied_speed = target_speed;
    }
}

void PlayerActivity::update_volume_osd_timeout() {
    if (!this->volume_osd_visible) {
        return;
    }

    if (this->controls_visible || this->overlay_visible) {
        this->volume_osd_visible = false;
        this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
        sync_overlay_to_surface();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (this->volume_osd_hide_time != std::chrono::steady_clock::time_point::min() &&
        now >= this->volume_osd_hide_time) {
        this->volume_osd_visible = false;
        this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
        sync_overlay_to_surface();
    }
}

void PlayerActivity::present_runtime_error_if_needed() {
    if (this->playback_error_dialog_open) {
        return;
    }

    std::string error_message = switchbox::core::switch_mpv_consume_last_error();
    if (error_message.empty()) {
        return;
    }

    this->playback_error_dialog_open = true;
    this->controls_visible = false;
    this->overlay_visible = false;
    this->volume_osd_visible = false;
    this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    sync_overlay_to_surface();
    stop_playback_session_before_leave();

    auto* dialog = new brls::Dialog(error_message);
    dialog->setCancelable(false);
    dialog->addButton(brls::getStr("hints/ok"), [this]() {
        this->playback_error_dialog_open = false;
        brls::Application::popActivity(brls::TransitionAnimation::FADE);
    });
    dialog->open();
}

void PlayerActivity::stop_playback_session_before_leave() {
    if (this->playback_session_stopped) {
        return;
    }

    this->playback_session_stopped = true;
    if (this->runtime_initialized) {
        this->runtime_tick.stop();
    }

    this->controls_visible = false;
    this->overlay_visible = false;
    this->volume_osd_visible = false;
    this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    sync_overlay_to_surface();
    switchbox::core::switch_mpv_stop();
}

void PlayerActivity::adjust_volume(int delta) {
    const int next_volume = std::clamp(this->session_volume + delta, 0, 100);
    if (next_volume == this->session_volume) {
        return;
    }

    if (switchbox::core::switch_mpv_set_volume(next_volume)) {
        this->session_volume = next_volume;
        this->player_volume_dirty = true;

        if (!this->controls_visible && !this->overlay_visible) {
            const int duration_ms = std::max(
                0,
                switchbox::core::AppConfigStore::current().general.player_volume_osd_duration_ms);
            this->volume_osd_visible = duration_ms > 0;
            this->volume_osd_hide_time =
                duration_ms > 0 ? (std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms))
                                : std::chrono::steady_clock::time_point::min();
        }

        sync_overlay_to_surface();
    }
}

bool PlayerActivity::seek_relative(double seconds) {
    return switchbox::core::switch_mpv_seek_relative_seconds(seconds);
}

void PlayerActivity::confirm_delete_current_file() {
    if (!this->has_smb_source || this->current_relative_path.empty()) {
        brls::Application::notify("Current source is not a deletable SMB file.");
        return;
    }

    const std::string deleting_file_name = file_name_from_relative_path(this->current_relative_path);
    const std::string confirm_message =
        deleting_file_name.empty()
            ? tr("player_page/delete/confirm_generic")
            : tr("player_page/delete/confirm_named", deleting_file_name);

    auto* dialog = new brls::Dialog(confirm_message);
    dialog->addButton(brls::getStr("hints/cancel"), []() {});
    dialog->addButton(brls::getStr("hints/ok"), [this]() {
        const switchbox::core::SmbSourceSettings source = this->smb_source;
        const std::string deleting_relative_path = this->current_relative_path;
        const std::string directory = switchbox::core::smb_parent_relative_path(this->current_relative_path);
        const std::string next_focus = find_next_focus_after_delete();
        stop_playback_session_before_leave();
        SmbBrowserActivity::request_delete_after_return(
            source,
            directory,
            next_focus,
            deleting_relative_path);
        brls::Application::popActivity(brls::TransitionAnimation::FADE);
    });
    dialog->open();
}

std::string PlayerActivity::find_next_focus_after_delete() const {
    if (!this->has_smb_source || this->current_relative_path.empty()) {
        return {};
    }

    const std::string directory = switchbox::core::smb_parent_relative_path(this->current_relative_path);
    const auto result = switchbox::core::browse_smb_directory(
        this->smb_source,
        switchbox::core::AppConfigStore::current().general,
        directory);
    if (!result.success || result.entries.empty()) {
        return {};
    }

    std::vector<std::string> files;
    files.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        if (!entry.is_directory) {
            files.push_back(entry.relative_path);
        }
    }

    if (files.empty()) {
        return {};
    }

    auto found = std::find(files.begin(), files.end(), this->current_relative_path);
    if (found == files.end()) {
        return files.front();
    }

    const size_t index = static_cast<size_t>(std::distance(files.begin(), found));
    if (index + 1 < files.size()) {
        return files[index + 1];
    }
    if (index > 0) {
        return files[index - 1];
    }
    return {};
}

switchbox::core::SmbSourceSettings PlayerActivity::make_smb_source_from_target() const {
    switchbox::core::SmbSourceSettings source;
    source.key = "runtime";
    source.title = this->target.source_label.empty() ? "SMB" : this->target.source_label;
    if (this->target.smb_locator.has_value()) {
        source.host = this->target.smb_locator->host;
        source.share = this->target.smb_locator->share;
        source.username = this->target.smb_locator->username;
        source.password = this->target.smb_locator->password;
    }
    return source;
}

}  // namespace switchbox::app
