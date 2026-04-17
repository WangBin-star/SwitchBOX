#include "switchbox/app/player_activity.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <borealis/core/application.hpp>
#include <borealis/core/box.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/dropdown.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/hint.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#if defined(__SWITCH__)
#include <borealis/platforms/switch/switch_video.hpp>
#endif

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/smb_browser_activity.hpp"
#include "switchbox/core/smb_browser.hpp"
#include "switchbox/core/smb2_mount_fs.hpp"
#include "switchbox/core/switch_mpv_player.hpp"

namespace switchbox::app {

namespace {

constexpr int kPlayerVerticalRepeatIntervalMs = 90;
constexpr int kPlayerControlsHorizontalRepeatIntervalMs = 90;
constexpr int kPlayerDirectionInitialRepeatDelayMs = 260;

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::string ascii_lower(std::string value) {
    for (char& character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return value;
}

std::string pick_target_locator(const switchbox::core::PlaybackTarget& target) {
    if (!target.primary_locator.empty()) {
        return target.primary_locator;
    }
    return target.fallback_locator;
}

std::string playback_source_kind_name(switchbox::core::PlaybackSourceKind kind) {
    switch (kind) {
        case switchbox::core::PlaybackSourceKind::Smb:
            return "smb";
        case switchbox::core::PlaybackSourceKind::Iptv:
            return "iptv";
        default:
            return "unknown";
    }
}

std::string detect_unsupported_iptv_web_platform(const switchbox::core::PlaybackTarget& target) {
    if (target.source_kind != switchbox::core::PlaybackSourceKind::Iptv) {
        return {};
    }

    const std::string locator = ascii_lower(pick_target_locator(target));
    if (locator.find("youtube.com") != std::string::npos || locator.find("youtu.be") != std::string::npos) {
        return "YouTube";
    }
    if (locator.find("twitch.tv") != std::string::npos) {
        return "Twitch";
    }

    return {};
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

bool ensure_smb_locator_on_target(switchbox::core::PlaybackTarget& target) {
    if (target.source_kind != switchbox::core::PlaybackSourceKind::Smb) {
        return false;
    }
    if (target.smb_locator.has_value()) {
        return true;
    }

    switchbox::core::PlaybackTarget::SmbLocator parsed_locator;
    if (!switchbox::core::try_parse_smb_locator_from_uri(pick_target_locator(target), parsed_locator)) {
        return false;
    }

    target.smb_locator = std::move(parsed_locator);
    return true;
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
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_DOWN,
        [this](brls::View*) { return handle_down_action(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_NAV_UP,
        [](brls::View*) { return true; },
        true,
        false,
        brls::SOUND_NONE);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_NAV_DOWN,
        [](brls::View*) { return true; },
        true,
        false,
        brls::SOUND_NONE);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_LEFT,
        [this](brls::View*) { return handle_left_action(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_RIGHT,
        [this](brls::View*) { return handle_right_action(); },
        true,
        false,
        brls::SOUND_CLICK);
    registerAction(
        tr("actions/disable"),
        brls::BUTTON_NAV_LEFT,
        [](brls::View*) { return true; },
        true,
        false,
        brls::SOUND_NONE);
    registerAction(
        tr("actions/enable"),
        brls::BUTTON_NAV_RIGHT,
        [](brls::View*) { return true; },
        true,
        false,
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
    switchbox::core::switch_mpv_append_debug_log_note("player_session_begin");
    switchbox::core::switch_mpv_append_debug_log_note(
        "activity_enter source_kind=" + playback_source_kind_name(this->target.source_kind) +
        " source_label=" + this->target.source_label +
        " title=" + this->target.title +
        " subtitle=" + this->target.subtitle +
        " display_locator=" + pick_target_locator(this->target));

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
        this->video_surface->set_progress_tap_handler([this](float ratio) {
            handle_touch_progress_tap(ratio);
        });
        this->video_surface->set_horizontal_pan_handler([this](brls::GestureState state, float delta_ratio) {
            handle_touch_horizontal_pan(state, delta_ratio);
        });
        this->video_surface->set_vertical_pan_handler([this](brls::GestureState state, float delta_ratio) {
            handle_touch_vertical_pan(state, delta_ratio);
        });
    }

    if (ensure_smb_locator_on_target(this->target)) {
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

void PlayerActivity::cancel_pending_startup_task() {
    if (this->pending_startup_task == nullptr) {
        return;
    }

    this->pending_startup_task->cancel_flag->store(true);
    this->pending_startup_task.reset();
}

void PlayerActivity::begin_async_startup_for_target(const switchbox::core::PlaybackTarget& next_target) {
    cancel_pending_startup_task();

    auto task = std::make_shared<PendingStartupTask>();
    {
        std::scoped_lock lock(task->mutex);
        task->prepared_target = next_target;
    }

    this->pending_startup_task = task;
    this->startup_loading_active = true;
    this->startup_loading_message = tr("player_page/loading/resolving_stream");
    this->startup_loading_started_at = std::chrono::steady_clock::now();
    update_startup_loading_overlay_state();
    switchbox::core::switch_mpv_append_debug_log_note("iptv_async_prepare begin");

    brls::async([task]() {
        switchbox::core::PlaybackTarget prepared_target;
        {
            std::scoped_lock lock(task->mutex);
            prepared_target = task->prepared_target;
        }

        if (task->cancel_flag->load()) {
            task->finished.store(true);
            return;
        }

        if (prepared_target.source_kind == switchbox::core::PlaybackSourceKind::Iptv &&
            !prepared_target.primary_locator.empty()) {
            const auto open_plan = switchbox::core::prepare_iptv_open_plan_for_playback(
                prepared_target,
                task->cancel_flag);
            if (!open_plan.success) {
                std::scoped_lock lock(task->mutex);
                task->startup_error =
                    open_plan.user_visible_reason.empty() ? "Unsupported IPTV source." : open_plan.user_visible_reason;
                task->finished.store(true);
                return;
            }

            prepared_target.primary_locator = open_plan.final_locator;
            prepared_target.http_user_agent = open_plan.effective_user_agent;
            prepared_target.http_referrer = open_plan.effective_referrer;
            prepared_target.http_header_fields = open_plan.effective_header_fields;
            prepared_target.iptv_open_plan = open_plan;
            prepared_target.locator_pre_resolved = true;
        }

        {
            std::scoped_lock lock(task->mutex);
            task->prepared_target = std::move(prepared_target);
        }
        task->finished.store(true);
    });
}

void PlayerActivity::apply_started_target_state(const switchbox::core::PlaybackTarget& next_target) {
    this->playback_session_stopped = false;
    this->touch_horizontal_pan_active = false;
    this->touch_vertical_pan_active = false;
    this->target = next_target;

    if (ensure_smb_locator_on_target(this->target)) {
        this->has_smb_source = true;
        this->smb_source = make_smb_source_from_target();
        this->current_relative_path = this->target.smb_locator->relative_path;
        this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->current_relative_path);
    } else {
        this->has_smb_source = false;
        this->smb_source = {};
        this->current_relative_path.clear();
        this->overlay_relative_path.clear();
        this->overlay_entries.clear();
        this->overlay_selected_index = -1;
        this->overlay_message.clear();
        this->overlay_visible = false;
    }

    this->applied_speed = 1.0;
    switchbox::core::switch_mpv_set_speed(1.0);
    switchbox::core::switch_mpv_set_volume(this->session_volume);
    refresh_overlay_entries(true);
}

void PlayerActivity::poll_pending_startup_task() {
    const auto task = this->pending_startup_task;
    if (task == nullptr || !task->finished.load()) {
        return;
    }

    this->pending_startup_task.reset();
    if (task->cancel_flag->load()) {
        update_startup_loading_overlay_state();
        return;
    }

    switchbox::core::PlaybackTarget prepared_target;
    std::string startup_error;
    {
        std::scoped_lock lock(task->mutex);
        prepared_target = task->prepared_target;
        startup_error = task->startup_error;
    }

    if (!startup_error.empty()) {
        this->startup_loading_active = false;
        this->startup_loading_overlay_visible = false;
        this->startup_loading_message.clear();
        this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
        sync_overlay_to_surface();
        queue_startup_dialog(startup_error, true, true);
        return;
    }

    switchbox::core::switch_mpv_append_debug_log_note("iptv_async_prepare ready");

    startup_error.clear();
    this->startup_loading_message = tr("player_page/loading/opening_stream");
    if (!switchbox::core::switch_mpv_open(prepared_target, startup_error) && !startup_error.empty()) {
        this->startup_loading_active = false;
        this->startup_loading_overlay_visible = false;
        this->startup_loading_message.clear();
        this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
        sync_overlay_to_surface();
        queue_startup_dialog(startup_error, true, true);
        return;
    }

    apply_started_target_state(prepared_target);
    update_startup_loading_overlay_state();
}

void PlayerActivity::update_startup_loading_overlay_state() {
    const bool previous_active = this->startup_loading_active;
    const bool previous_visible = this->startup_loading_overlay_visible;
    const std::string previous_message = this->startup_loading_message;
    const auto previous_started_at = this->startup_loading_started_at;
    const auto now = std::chrono::steady_clock::now();

    if (this->playback_error_dialog_open || this->startup_dialog_pending) {
        this->startup_loading_active = false;
        this->startup_loading_message.clear();
        this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    } else if (this->pending_startup_task != nullptr) {
        this->startup_loading_active = true;
        if (this->startup_loading_started_at == std::chrono::steady_clock::time_point::min()) {
            this->startup_loading_started_at = now;
        }
        this->startup_loading_message = tr("player_page/loading/resolving_stream");
    } else if (this->target.source_kind == switchbox::core::PlaybackSourceKind::Iptv &&
               switchbox::core::switch_mpv_session_active() &&
               !switchbox::core::switch_mpv_has_rendered_video_frame()) {
        this->startup_loading_active = true;
        if (this->startup_loading_started_at == std::chrono::steady_clock::time_point::min()) {
            this->startup_loading_started_at = now;
        }
        this->startup_loading_message = switchbox::core::switch_mpv_has_media()
                                            ? tr("player_page/loading/waiting_first_frame")
                                            : tr("player_page/loading/opening_stream");
    } else {
        this->startup_loading_active = false;
        this->startup_loading_message.clear();
        this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    }

    if (!this->startup_loading_active) {
        this->startup_loading_overlay_visible = false;
    } else {
        const int delay_ms = std::max(
            0,
            switchbox::core::AppConfigStore::current().general.player_loading_overlay_delay_ms);
        this->startup_loading_overlay_visible =
            delay_ms == 0 ||
            (this->startup_loading_started_at != std::chrono::steady_clock::time_point::min() &&
             now >= this->startup_loading_started_at + std::chrono::milliseconds(delay_ms));
    }

    if (previous_active != this->startup_loading_active ||
        previous_visible != this->startup_loading_overlay_visible ||
        previous_message != this->startup_loading_message ||
        previous_started_at != this->startup_loading_started_at) {
        sync_overlay_to_surface();
    }
}

void PlayerActivity::start_playback_with_target(const switchbox::core::PlaybackTarget& next_target) {
    const bool has_existing_playback_session =
        switchbox::core::switch_mpv_session_active() || switchbox::core::switch_mpv_has_media();
    const bool switching_inside_player =
        !this->playback_session_stopped &&
        (has_existing_playback_session || this->pending_startup_task != nullptr);
    const bool return_to_previous_activity_on_error = !switching_inside_player;
    const bool full_backend_restart_for_switch =
        switching_inside_player &&
        has_existing_playback_session &&
        this->target.source_kind == switchbox::core::PlaybackSourceKind::Smb &&
        next_target.source_kind == switchbox::core::PlaybackSourceKind::Smb;

    switchbox::core::switch_mpv_append_debug_log_note(
        "start_playback source_kind=" + playback_source_kind_name(next_target.source_kind) +
        " source_label=" + next_target.source_label +
        " title=" + next_target.title +
        " subtitle=" + next_target.subtitle +
        " locator=" + pick_target_locator(next_target));

    if (const std::string platform = detect_unsupported_iptv_web_platform(next_target); !platform.empty()) {
        queue_startup_dialog(
            tr("player_page/errors/unsupported_web_stream", platform),
            return_to_previous_activity_on_error,
            false);
        return;
    }

    cancel_pending_startup_task();
    if (full_backend_restart_for_switch) {
        switchbox::core::switch_mpv_append_debug_log_note("start_playback restart_backend_for_switch");
        switchbox::core::switch_mpv_shutdown();
        switchbox::core::switch_smb_mount_release();
    } else if (has_existing_playback_session) {
        switchbox::core::switch_mpv_append_debug_log_note("start_playback stop_previous_session");
        switchbox::core::switch_mpv_stop();
        (void)switchbox::core::switch_mpv_consume_last_error();
    }

    std::string startup_error;
    if (!prepare_switch_renderer_if_needed(startup_error) && !startup_error.empty()) {
        queue_startup_dialog(startup_error, return_to_previous_activity_on_error, true);
        return;
    }

    if (next_target.source_kind == switchbox::core::PlaybackSourceKind::Iptv) {
        begin_async_startup_for_target(next_target);
        return;
    }

    if (!switchbox::core::switch_mpv_open(next_target, startup_error) && !startup_error.empty()) {
        queue_startup_dialog(startup_error, return_to_previous_activity_on_error, true);
        return;
    }

    this->startup_loading_active = false;
    this->startup_loading_overlay_visible = false;
    this->startup_loading_message.clear();
    this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    apply_started_target_state(next_target);
}

void PlayerActivity::queue_startup_dialog(
    std::string message,
    bool return_to_previous_activity,
    bool stop_playback_before_dialog) {
    switchbox::core::switch_mpv_append_debug_log_note(
        "startup_dialog queued return=" + std::string(return_to_previous_activity ? "true" : "false") +
        " stop_before_open=" + std::string(stop_playback_before_dialog ? "true" : "false") +
        " message=" + message);
    this->startup_dialog_message = std::move(message);
    this->startup_dialog_pending = !this->startup_dialog_message.empty();
    this->startup_dialog_returns_to_previous = return_to_previous_activity;
    this->startup_dialog_stops_playback_before_open = stop_playback_before_dialog;
}

void PlayerActivity::present_startup_dialog_if_needed() {
    if (!this->startup_dialog_pending || this->playback_error_dialog_open) {
        return;
    }

    this->startup_dialog_pending = false;
    this->playback_error_dialog_open = true;
    this->startup_loading_active = false;
    this->startup_loading_overlay_visible = false;
    this->startup_loading_message.clear();
    this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    this->controls_visible = false;
    this->overlay_visible = false;
    this->volume_osd_visible = false;
    this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    sync_overlay_to_surface();

    if (this->startup_dialog_stops_playback_before_open) {
        stop_playback_session_before_leave();
    }

    const bool return_to_previous_activity = this->startup_dialog_returns_to_previous;
    auto* dialog = new brls::Dialog(this->startup_dialog_message);
    dialog->setCancelable(false);
    dialog->addButton(brls::getStr("hints/ok"), [this, return_to_previous_activity]() {
        this->playback_error_dialog_open = false;
        if (return_to_previous_activity) {
            dismiss_to_previous_activity_if_still_top();
        }
    });
    dialog->open();
}

void PlayerActivity::dismiss_to_previous_activity_if_still_top() {
    const auto activities = brls::Application::getActivitiesStack();
    if (activities.empty() || activities.back() != this) {
        return;
    }

    brls::Application::popActivity(
        brls::TransitionAnimation::FADE,
        []() {
            const auto remaining_activities = brls::Application::getActivitiesStack();
            if (remaining_activities.empty()) {
                return;
            }

            auto* current_activity = remaining_activities.back();
            auto* current_focus = brls::Application::getCurrentFocus();
            if (current_focus != nullptr && current_focus->getParentActivity() == current_activity) {
                return;
            }

            if (auto* fallback_focus = current_activity->getDefaultFocus()) {
                brls::Application::giveFocus(fallback_focus);
            }
        });
}

bool PlayerActivity::prepare_switch_renderer_if_needed(std::string& error_message) {
    error_message.clear();

#if defined(__SWITCH__)
    auto* switch_video_context = dynamic_cast<brls::SwitchVideoContext*>(
        brls::Application::getPlatform()->getVideoContext());
    if (switch_video_context == nullptr) {
        return true;
    }

    const int width = std::max(1, static_cast<int>(brls::Application::windowWidth));
    const int height = std::max(1, static_cast<int>(brls::Application::windowHeight));
    return switchbox::core::switch_mpv_prepare_renderer_for_switch(
        switch_video_context->getDeko3dDevice(),
        switch_video_context->getQueue(),
        width,
        height,
        error_message);
#else
    return true;
#endif
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
    switchbox::core::switch_mpv_append_debug_log_note("input_b pressed");
    if (this->playback_error_dialog_open || this->startup_dialog_pending) {
        return true;
    }

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
    dismiss_to_previous_activity_if_still_top();
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
    const auto& controller = brls::Application::getControllerState();
    if (this->controls_visible) {
        if (controller.buttons[brls::BUTTON_LB] ||
            controller.buttons[brls::BUTTON_RB] ||
            controller.buttons[brls::BUTTON_LT] ||
            controller.buttons[brls::BUTTON_RT]) {
            return true;
        }
        if (should_accept_controls_navigation_step(-1)) {
            move_controls_selection(-1);
        }
        return true;
    }

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
    const auto& controller = brls::Application::getControllerState();
    if (this->controls_visible) {
        if (controller.buttons[brls::BUTTON_LB] ||
            controller.buttons[brls::BUTTON_RB] ||
            controller.buttons[brls::BUTTON_LT] ||
            controller.buttons[brls::BUTTON_RT]) {
            return true;
        }
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
    const auto& controller = brls::Application::getControllerState();
    if (direction_left_pressed(controller) || direction_right_pressed(controller)) {
        return true;
    }

    return seek_relative(-static_cast<double>(switchbox::core::AppConfigStore::current().general.short_seek));
}

bool PlayerActivity::handle_short_forward() {
    const auto& controller = brls::Application::getControllerState();
    if (direction_left_pressed(controller) || direction_right_pressed(controller)) {
        return true;
    }

    return seek_relative(static_cast<double>(switchbox::core::AppConfigStore::current().general.short_seek));
}

bool PlayerActivity::handle_long_backward() {
    return seek_relative(-static_cast<double>(switchbox::core::AppConfigStore::current().general.long_seek));
}

bool PlayerActivity::handle_long_forward() {
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
    model.loading_overlay_visible = this->startup_loading_overlay_visible;
    model.loading_overlay_title = tr("player_page/loading/title");
    model.loading_overlay_message = this->startup_loading_message;
    model.overlay_marquee_delay_ms = general.overlay_marquee_delay_ms;
    model.touch_enable = general.touch_enable;
    model.touch_player_gestures = general.touch_player_gestures;
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

void PlayerActivity::ensure_controls_panel_visible_for_touch() {
    if (!this->controls_visible) {
        this->controls_visible = true;
    }

    this->last_controls_repeat_action = -1;
    this->last_controls_repeat_time = std::chrono::steady_clock::time_point::min();
    this->last_controls_nav_direction = 0;
    this->last_controls_nav_time = std::chrono::steady_clock::time_point::min();
    this->last_controls_horizontal_repeat_direction = 0;
    this->last_controls_horizontal_repeat_time = std::chrono::steady_clock::time_point::min();
    this->volume_osd_visible = false;
    this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();

    if (this->overlay_visible) {
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
        next = std::clamp(next + delta, 0, kButtonCount - 1);
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
    poll_pending_startup_task();
    update_startup_loading_overlay_state();
    present_startup_dialog_if_needed();
    if (this->playback_error_dialog_open) {
        return;
    }

    apply_directional_input_fallback_if_needed();
    apply_vertical_repeat_if_needed();
    apply_controls_horizontal_repeat_if_needed();
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

void PlayerActivity::apply_vertical_repeat_if_needed() {
    const auto& controller = brls::Application::getControllerState();

    const bool up_pressed =
        controller.buttons[brls::BUTTON_UP] ||
        controller.buttons[brls::BUTTON_NAV_UP] ||
        left_stick_up_pressed(controller) ||
        right_stick_up_pressed(controller);
    const bool down_pressed =
        controller.buttons[brls::BUTTON_DOWN] ||
        controller.buttons[brls::BUTTON_NAV_DOWN] ||
        left_stick_down_pressed(controller) ||
        right_stick_down_pressed(controller);

    int direction = 0;
    if (up_pressed != down_pressed) {
        direction = up_pressed ? -1 : 1;
    }

    if (direction == 0) {
        this->last_vertical_repeat_direction = 0;
        this->last_vertical_repeat_time = std::chrono::steady_clock::time_point::min();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (direction != this->last_vertical_repeat_direction) {
        this->last_vertical_repeat_direction = direction;
        this->last_vertical_repeat_time = now + std::chrono::milliseconds(kPlayerDirectionInitialRepeatDelayMs);
        return;
    }

    if (this->last_vertical_repeat_time != std::chrono::steady_clock::time_point::min() &&
        now < this->last_vertical_repeat_time) {
        return;
    }

    this->last_vertical_repeat_time = now + std::chrono::milliseconds(kPlayerVerticalRepeatIntervalMs);
    if (direction < 0) {
        handle_up_action();
    } else {
        handle_down_action();
    }
}

void PlayerActivity::apply_controls_horizontal_repeat_if_needed() {
    if (!this->controls_visible) {
        this->last_controls_horizontal_repeat_direction = 0;
        this->last_controls_horizontal_repeat_time = std::chrono::steady_clock::time_point::min();
        return;
    }

    const auto& controller = brls::Application::getControllerState();
    if (controller.buttons[brls::BUTTON_LB] ||
        controller.buttons[brls::BUTTON_RB] ||
        controller.buttons[brls::BUTTON_LT] ||
        controller.buttons[brls::BUTTON_RT]) {
        this->last_controls_horizontal_repeat_direction = 0;
        this->last_controls_horizontal_repeat_time = std::chrono::steady_clock::time_point::min();
        return;
    }

    const bool left_pressed =
        controller.buttons[brls::BUTTON_LEFT] ||
        controller.buttons[brls::BUTTON_NAV_LEFT] ||
        left_stick_left_pressed(controller) ||
        right_stick_left_pressed(controller);
    const bool right_pressed =
        controller.buttons[brls::BUTTON_RIGHT] ||
        controller.buttons[brls::BUTTON_NAV_RIGHT] ||
        left_stick_right_pressed(controller) ||
        right_stick_right_pressed(controller);

    int direction = 0;
    if (left_pressed != right_pressed) {
        direction = left_pressed ? -1 : 1;
    }

    if (direction == 0) {
        this->last_controls_horizontal_repeat_direction = 0;
        this->last_controls_horizontal_repeat_time = std::chrono::steady_clock::time_point::min();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (direction != this->last_controls_horizontal_repeat_direction) {
        this->last_controls_horizontal_repeat_direction = direction;
        this->last_controls_horizontal_repeat_time =
            now + std::chrono::milliseconds(kPlayerDirectionInitialRepeatDelayMs);
        return;
    }

    if (this->last_controls_horizontal_repeat_time != std::chrono::steady_clock::time_point::min() &&
        now < this->last_controls_horizontal_repeat_time) {
        return;
    }

    this->last_controls_horizontal_repeat_time =
        now + std::chrono::milliseconds(kPlayerControlsHorizontalRepeatIntervalMs);
    if (direction < 0) {
        handle_left_action();
    } else {
        handle_right_action();
    }
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
    if (this->overlay_visible) {
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

    if (switchbox::core::switch_mpv_session_active() || switchbox::core::switch_mpv_has_media()) {
        switchbox::core::switch_mpv_append_debug_log_note(
            "runtime_error_ignored_while_session_active message=" + error_message);
        return;
    }

    this->playback_error_dialog_open = true;
    this->startup_loading_active = false;
    this->startup_loading_overlay_visible = false;
    this->startup_loading_message.clear();
    this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
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
        dismiss_to_previous_activity_if_still_top();
    });
    dialog->open();
}

void PlayerActivity::stop_playback_session_before_leave() {
    if (this->playback_session_stopped) {
        return;
    }

    switchbox::core::switch_mpv_append_debug_log_note("stop_before_leave begin");
    this->playback_session_stopped = true;
    cancel_pending_startup_task();
    if (this->runtime_initialized) {
        this->runtime_tick.stop();
    }

    this->startup_loading_active = false;
    this->startup_loading_overlay_visible = false;
    this->startup_loading_message.clear();
    this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    this->controls_visible = false;
    this->overlay_visible = false;
    this->volume_osd_visible = false;
    this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    sync_overlay_to_surface();
    switchbox::core::switch_mpv_stop();
    switchbox::core::switch_mpv_append_debug_log_note("stop_before_leave end");
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

bool PlayerActivity::seek_absolute(double seconds) {
    return switchbox::core::switch_mpv_seek_absolute_seconds(seconds);
}

void PlayerActivity::handle_touch_horizontal_pan(brls::GestureState state, float delta_ratio) {
    const auto& general = switchbox::core::AppConfigStore::current().general;
    if (!general.touch_enable || !general.touch_player_gestures) {
        this->touch_horizontal_pan_active = false;
        return;
    }

    if (!this->touch_horizontal_pan_active) {
        this->touch_horizontal_pan_active = true;
        this->touch_seek_anchor_seconds = switchbox::core::switch_mpv_get_position_seconds();
        ensure_controls_panel_visible_for_touch();
    }

    if (!this->touch_horizontal_pan_active) {
        return;
    }

    const double duration = switchbox::core::switch_mpv_get_duration_seconds();
    if (duration > 0.0) {
        const double target_seconds =
            std::clamp(this->touch_seek_anchor_seconds + static_cast<double>(delta_ratio) * duration, 0.0, duration);
        (void)seek_absolute(target_seconds);
    }

    if (state == brls::GestureState::END) {
        this->touch_horizontal_pan_active = false;
    }
}

void PlayerActivity::handle_touch_vertical_pan(brls::GestureState state, float delta_ratio) {
    const auto& general = switchbox::core::AppConfigStore::current().general;
    if (!general.touch_enable || !general.touch_player_gestures) {
        this->touch_vertical_pan_active = false;
        return;
    }

    if (!this->touch_vertical_pan_active) {
        this->touch_vertical_pan_active = true;
        this->touch_volume_anchor = this->session_volume;
    }

    if (!this->touch_vertical_pan_active) {
        return;
    }

    const int target_volume = std::clamp(
        this->touch_volume_anchor + static_cast<int>(std::lround(delta_ratio * 100.0f)),
        0,
        100);
    adjust_volume(target_volume - this->session_volume);

    if (state == brls::GestureState::END) {
        this->touch_vertical_pan_active = false;
    }
}

void PlayerActivity::handle_touch_progress_tap(float ratio) {
    const auto& general = switchbox::core::AppConfigStore::current().general;
    if (!general.touch_enable || !general.touch_player_gestures || !this->controls_visible) {
        return;
    }

    const double duration = switchbox::core::switch_mpv_get_duration_seconds();
    if (duration <= 0.0) {
        return;
    }

    ensure_controls_panel_visible_for_touch();
    (void)seek_absolute(std::clamp(static_cast<double>(ratio), 0.0, 1.0) * duration);
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
        dismiss_to_previous_activity_if_still_top();
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
        return source;
    }

    switchbox::core::PlaybackTarget::SmbLocator parsed_locator;
    if (switchbox::core::try_parse_smb_locator_from_uri(pick_target_locator(this->target), parsed_locator)) {
        source.host = parsed_locator.host;
        source.share = parsed_locator.share;
        source.username = parsed_locator.username;
        source.password = parsed_locator.password;
    }
    return source;
}

}  // namespace switchbox::app
