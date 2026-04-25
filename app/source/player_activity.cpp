#include "switchbox/app/player_activity.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
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
#include <switch/services/applet.h>
#endif

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/smb_browser_activity.hpp"
#include "switchbox/app/webdav_browser_activity.hpp"
#include "switchbox/core/playback_history.hpp"
#include "switchbox/core/smb_browser.hpp"
#include "switchbox/core/smb2_mount_fs.hpp"
#include "switchbox/core/switch_mpv_player.hpp"
#include "switchbox/core/webdav_browser.hpp"

namespace switchbox::app {

namespace {

constexpr int kPlayerVerticalRepeatIntervalMs = 90;
constexpr int kPlayerControlsHorizontalRepeatIntervalMs = 90;
constexpr int kResumePersistIntervalMs = 5000;
constexpr int kResumeRestoreTimeoutMs = 15000;
std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::string build_error_dialog_message(const std::string& summary, const std::string& detail) {
    if (detail.empty()) {
        return summary;
    }

    return summary + "\n\n" + detail;
}

std::string startup_loading_message_for_prepare_stage(switchbox::core::IptvOpenPlanStage stage) {
    switch (stage) {
        case switchbox::core::IptvOpenPlanStage::NormalizingLocator:
            return tr("player_page/loading/normalizing_locator");
        case switchbox::core::IptvOpenPlanStage::ProbingSource:
            return tr("player_page/loading/probing_source");
        case switchbox::core::IptvOpenPlanStage::InspectingResponse:
            return tr("player_page/loading/inspecting_response");
        case switchbox::core::IptvOpenPlanStage::ResolvingPlaylist:
            return tr("player_page/loading/resolving_playlist");
        case switchbox::core::IptvOpenPlanStage::ProbingVariant:
            return tr("player_page/loading/probing_variant");
        case switchbox::core::IptvOpenPlanStage::FinalizingPlan:
            return tr("player_page/loading/finalizing_plan");
        case switchbox::core::IptvOpenPlanStage::Starting:
        default:
            return tr("player_page/loading/resolving_stream");
    }
}

std::string startup_loading_detail_for_prepare_stage(switchbox::core::IptvOpenPlanStage stage) {
    switch (stage) {
        case switchbox::core::IptvOpenPlanStage::NormalizingLocator:
            return tr("player_page/loading_details/normalizing_locator");
        case switchbox::core::IptvOpenPlanStage::ProbingSource:
            return tr("player_page/loading_details/probing_source");
        case switchbox::core::IptvOpenPlanStage::InspectingResponse:
            return tr("player_page/loading_details/inspecting_response");
        case switchbox::core::IptvOpenPlanStage::ResolvingPlaylist:
            return tr("player_page/loading_details/resolving_playlist");
        case switchbox::core::IptvOpenPlanStage::ProbingVariant:
            return tr("player_page/loading_details/probing_variant");
        case switchbox::core::IptvOpenPlanStage::FinalizingPlan:
            return tr("player_page/loading_details/finalizing_plan");
        case switchbox::core::IptvOpenPlanStage::Starting:
        default:
            return tr("player_page/loading_details/resolving_stream");
    }
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
        case switchbox::core::PlaybackSourceKind::WebDav:
            return "webdav";
        default:
            return "unknown";
    }
}

std::string startup_loading_title_for_source_kind(switchbox::core::PlaybackSourceKind kind) {
    if (kind == switchbox::core::PlaybackSourceKind::Iptv) {
        return tr("player_page/loading/title");
    }

    return tr("player_page/loading/file_title");
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
    this->button_long_press_threshold_ms = switchbox::core::ini_only_button_long_press_threshold_ms();
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
        tr("actions/speed_mode"),
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
    switchbox::core::switch_mpv_begin_debug_log_session();
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
        this->video_surface->set_double_tap_handler([this]() {
            handle_touch_double_tap();
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
        this->has_webdav_source = false;
        this->webdav_source = {};
        this->current_relative_path = this->target.smb_locator->relative_path;
        this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->current_relative_path);
    } else if (this->target.webdav_locator.has_value()) {
        this->has_smb_source = false;
        this->smb_source = {};
        this->has_webdav_source = true;
        this->webdav_source = make_webdav_source_from_target();
        this->current_relative_path = this->target.webdav_locator->relative_path;
        this->overlay_relative_path = switchbox::core::webdav_parent_relative_path(this->current_relative_path);
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
    this->startup_loading_detail = tr("player_page/loading_details/resolving_stream");
    this->startup_loading_progress = 0.10f;
    this->startup_loading_started_at = std::chrono::steady_clock::now();
    update_startup_loading_overlay_state();
    switchbox::core::switch_mpv_append_debug_log_note(
        "async_prepare begin source_kind=" + playback_source_kind_name(next_target.source_kind));

    const std::string webdav_prepare_message = tr("player_page/loading/preparing_file");
    const std::string webdav_prepare_detail = tr("player_page/loading_details/preparing_file");
    const std::string webdav_verify_message = tr("player_page/loading/verifying_file");
    const std::string webdav_verify_detail = tr("player_page/loading_details/verifying_file");
    const std::string webdav_open_failed_summary = tr("webdav_browser/open_failed");

    brls::async([task,
                 webdav_prepare_message,
                 webdav_prepare_detail,
                 webdav_verify_message,
                 webdav_verify_detail,
                 webdav_open_failed_summary]() {
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
                task->cancel_flag,
                [task](const switchbox::core::IptvOpenPlanProgress& progress) {
                    std::scoped_lock lock(task->mutex);
                    task->progress = progress;
                });
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
        } else if (prepared_target.source_kind == switchbox::core::PlaybackSourceKind::WebDav &&
                   prepared_target.webdav_locator.has_value()) {
            {
                std::scoped_lock lock(task->mutex);
                task->use_custom_loading_state = true;
                task->loading_message = webdav_prepare_message;
                task->loading_detail = webdav_prepare_detail;
                task->loading_progress = 0.18f;
            }

            if (task->cancel_flag->load()) {
                task->finished.store(true);
                return;
            }

            {
                std::scoped_lock lock(task->mutex);
                task->loading_message = webdav_verify_message;
                task->loading_detail = webdav_verify_detail;
                task->loading_progress = 0.56f;
            }

            const auto probe_result = switchbox::core::probe_webdav_file(
                switchbox::core::WebDavSourceSettings{
                    .key = prepared_target.source_key,
                    .title = prepared_target.source_label,
                    .url = prepared_target.webdav_locator->url,
                    .username = prepared_target.webdav_locator->username,
                    .password = prepared_target.webdav_locator->password,
                },
                prepared_target.webdav_locator->relative_path);
            if (!probe_result.success) {
                std::scoped_lock lock(task->mutex);
                task->startup_error = probe_result.error_message.empty()
                                          ? webdav_open_failed_summary
                                          : build_error_dialog_message(
                                                webdav_open_failed_summary,
                                                probe_result.error_message);
                task->finished.store(true);
                return;
            }
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
    this->playback_history_recorded_for_target = false;
    this->playback_history_restart_at_ms = 0;
    this->resume_restore_pending = false;
    this->resume_restore_position_seconds = 0.0;
    this->resume_restore_started_at = std::chrono::steady_clock::time_point::min();
    this->last_resume_persist_time = std::chrono::steady_clock::time_point::min();
    this->touch_horizontal_pan_active = false;
    this->touch_vertical_pan_active = false;
    this->target = next_target;
    this->iptv_overlay_context = this->target.iptv_overlay_context;
    this->iptv_overlay_group_index = this->target.iptv_overlay_group_index;
    this->iptv_overlay_group_picker = false;

    if (ensure_smb_locator_on_target(this->target)) {
        this->has_smb_source = true;
        this->smb_source = make_smb_source_from_target();
        this->has_webdav_source = false;
        this->webdav_source = {};
        this->current_relative_path = this->target.smb_locator->relative_path;
        this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->current_relative_path);
    } else if (this->target.webdav_locator.has_value()) {
        this->has_smb_source = false;
        this->smb_source = {};
        this->has_webdav_source = true;
        this->webdav_source = make_webdav_source_from_target();
        this->current_relative_path = this->target.webdav_locator->relative_path;
        this->overlay_relative_path = switchbox::core::webdav_parent_relative_path(this->current_relative_path);
    } else {
        this->has_smb_source = false;
        this->smb_source = {};
        this->has_webdav_source = false;
        this->webdav_source = {};
        this->current_relative_path.clear();
        this->overlay_relative_path.clear();
    }

    if (this->has_smb_source || this->has_webdav_source) {
        this->iptv_overlay_context.reset();
        this->iptv_overlay_group_index = 0;
        this->iptv_overlay_group_picker = false;
    }

    if (!this->has_smb_source && !this->has_webdav_source && this->iptv_overlay_context == nullptr) {
        this->overlay_entries.clear();
        this->overlay_selected_index = -1;
        this->overlay_message.clear();
        this->overlay_visible = false;
    }

    this->applied_speed = 1.0;
    switchbox::core::switch_mpv_set_speed(1.0);
    apply_hold_speed_if_needed();
    switchbox::core::switch_mpv_set_volume(this->session_volume);
    refresh_resume_restore_state_for_target();
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
        this->startup_loading_detail.clear();
        this->startup_loading_progress = 0.0f;
        this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
        sync_overlay_to_surface();
        queue_startup_dialog(startup_error, true, true);
        return;
    }

    switchbox::core::switch_mpv_append_debug_log_note(
        "async_prepare ready source_kind=" + playback_source_kind_name(prepared_target.source_kind));

    startup_error.clear();
    this->startup_loading_message = tr("player_page/loading/opening_stream");
    this->startup_loading_detail = tr("player_page/loading_details/opening_stream");
    this->startup_loading_progress = 0.84f;
    sync_overlay_to_surface();
    if (!switchbox::core::switch_mpv_open(prepared_target, startup_error) && !startup_error.empty()) {
        this->startup_loading_active = false;
        this->startup_loading_overlay_visible = false;
        this->startup_loading_message.clear();
        this->startup_loading_detail.clear();
        this->startup_loading_progress = 0.0f;
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
    const std::string previous_detail = this->startup_loading_detail;
    const float previous_progress = this->startup_loading_progress;
    const auto previous_started_at = this->startup_loading_started_at;
    const auto now = std::chrono::steady_clock::now();

    if (this->playback_error_dialog_open || this->startup_dialog_pending) {
        this->startup_loading_active = false;
        this->startup_loading_message.clear();
        this->startup_loading_detail.clear();
        this->startup_loading_progress = 0.0f;
        this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    } else if (this->pending_startup_task != nullptr) {
        this->startup_loading_active = true;
        if (this->startup_loading_started_at == std::chrono::steady_clock::time_point::min()) {
            this->startup_loading_started_at = now;
        }
        switchbox::core::IptvOpenPlanProgress prepare_progress;
        bool use_custom_loading_state = false;
        std::string custom_loading_message;
        std::string custom_loading_detail;
        float custom_loading_progress = 0.0f;
        {
            std::scoped_lock lock(this->pending_startup_task->mutex);
            prepare_progress = this->pending_startup_task->progress;
            use_custom_loading_state = this->pending_startup_task->use_custom_loading_state;
            custom_loading_message = this->pending_startup_task->loading_message;
            custom_loading_detail = this->pending_startup_task->loading_detail;
            custom_loading_progress = this->pending_startup_task->loading_progress;
        }
        if (use_custom_loading_state) {
            this->startup_loading_message = custom_loading_message;
            this->startup_loading_detail = custom_loading_detail;
            this->startup_loading_progress = std::clamp(custom_loading_progress, 0.10f, 0.78f);
        } else {
            this->startup_loading_message = startup_loading_message_for_prepare_stage(prepare_progress.stage);
            this->startup_loading_detail = startup_loading_detail_for_prepare_stage(prepare_progress.stage);
            this->startup_loading_progress = std::max(0.10f, std::clamp(prepare_progress.progress, 0.0f, 0.78f));
        }
    } else if ((this->target.source_kind == switchbox::core::PlaybackSourceKind::Iptv ||
                this->target.source_kind == switchbox::core::PlaybackSourceKind::WebDav) &&
               switchbox::core::switch_mpv_session_active() &&
               !switchbox::core::switch_mpv_has_rendered_video_frame()) {
        this->startup_loading_active = true;
        if (this->startup_loading_started_at == std::chrono::steady_clock::time_point::min()) {
            this->startup_loading_started_at = now;
        }
        if (switchbox::core::switch_mpv_has_media()) {
            this->startup_loading_message = tr("player_page/loading/waiting_first_frame");
            this->startup_loading_detail = tr("player_page/loading_details/waiting_first_frame");
            const float elapsed_seconds = std::chrono::duration<float>(now - this->startup_loading_started_at).count();
            this->startup_loading_progress = std::min(0.98f, 0.93f + std::max(0.0f, elapsed_seconds) * 0.01f);
        } else {
            this->startup_loading_message = tr("player_page/loading/waiting_stream_metadata");
            this->startup_loading_detail = tr("player_page/loading_details/waiting_stream_metadata");
            this->startup_loading_progress = 0.88f;
        }
    } else {
        this->startup_loading_active = false;
        this->startup_loading_message.clear();
        this->startup_loading_detail.clear();
        this->startup_loading_progress = 0.0f;
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
        previous_detail != this->startup_loading_detail ||
        std::fabs(previous_progress - this->startup_loading_progress) > 0.0001f ||
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
        ((this->target.source_kind == switchbox::core::PlaybackSourceKind::Smb &&
          next_target.source_kind == switchbox::core::PlaybackSourceKind::Smb) ||
         (this->target.source_kind == switchbox::core::PlaybackSourceKind::WebDav &&
          next_target.source_kind == switchbox::core::PlaybackSourceKind::WebDav));

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
    if (has_existing_playback_session) {
        flush_playback_resume_state(true);
    }
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

    if (next_target.source_kind == switchbox::core::PlaybackSourceKind::Iptv ||
        next_target.source_kind == switchbox::core::PlaybackSourceKind::WebDav) {
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
    this->startup_loading_detail.clear();
    this->startup_loading_progress = 0.0f;
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
    this->startup_loading_detail.clear();
    this->startup_loading_progress = 0.0f;
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
            if (auto* smb_activity = dynamic_cast<SmbBrowserActivity*>(current_activity)) {
                smb_activity->apply_pending_return_from_player_if_any();
            } else if (auto* webdav_activity = dynamic_cast<WebDavBrowserActivity*>(current_activity)) {
                webdav_activity->apply_pending_return_from_player_if_any();
            }

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

    if (this->has_smb_source && !this->current_relative_path.empty()) {
        SmbBrowserActivity::request_focus_after_return(
            this->smb_source,
            switchbox::core::smb_parent_relative_path(this->current_relative_path),
            this->current_relative_path);
    } else if (this->has_webdav_source && !this->current_relative_path.empty()) {
        WebDavBrowserActivity::request_focus_after_return(
            this->webdav_source,
            switchbox::core::webdav_parent_relative_path(this->current_relative_path),
            this->current_relative_path);
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
    if (!this->y_button_down) {
        this->y_button_down = true;
        this->y_hold_speed_active = false;
        this->y_button_pressed_at = std::chrono::steady_clock::now();
    }
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
    std::string selected_key;
    if (keep_selection &&
        this->overlay_selected_index >= 0 &&
        this->overlay_selected_index < static_cast<int>(this->overlay_entries.size())) {
        selected_key = this->overlay_entries[static_cast<size_t>(this->overlay_selected_index)].stable_key;
    }

    this->overlay_message.clear();
    this->overlay_entries.clear();

    if (!this->has_smb_source && !this->has_webdav_source && this->iptv_overlay_context == nullptr) {
        sync_overlay_to_surface();
        return;
    }

    if (this->has_smb_source) {
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

        this->overlay_entries.reserve(result.entries.size());
        for (const auto& entry : result.entries) {
            this->overlay_entries.push_back({
                .title = entry.name,
                .stable_key = entry.relative_path,
                .is_directory = entry.is_directory,
                .is_current = entry.relative_path == this->current_relative_path,
                .smb_relative_path = entry.relative_path,
                .webdav_relative_path = {},
            });
        }
    } else if (this->has_webdav_source) {
        const auto& config = switchbox::core::AppConfigStore::current();
        const auto result =
            switchbox::core::browse_webdav_directory(
                this->webdav_source,
                config.general,
                this->overlay_relative_path);
        if (!result.success) {
            this->overlay_message = result.error_message;
            this->overlay_selected_index = -1;
            sync_overlay_to_surface();
            return;
        }

        this->overlay_entries.reserve(result.entries.size());
        for (const auto& entry : result.entries) {
            this->overlay_entries.push_back({
                .title = entry.name,
                .stable_key = entry.relative_path,
                .is_directory = entry.is_directory,
                .is_current = entry.relative_path == this->current_relative_path,
                .smb_relative_path = {},
                .webdav_relative_path = entry.relative_path,
            });
        }
    } else if (this->iptv_overlay_context != nullptr) {
        const auto group_count = this->iptv_overlay_context->groups.size();
        if (group_count == 0) {
            this->overlay_selected_index = -1;
            sync_overlay_to_surface();
            return;
        }

        this->iptv_overlay_group_index = std::min(this->iptv_overlay_group_index, group_count - 1);
        if (this->iptv_overlay_group_picker) {
            this->overlay_entries.reserve(group_count);
            for (size_t group_index = 0; group_index < group_count; ++group_index) {
                const auto& group = this->iptv_overlay_context->groups[group_index];
                this->overlay_entries.push_back({
                    .title = group.title,
                    .stable_key = "group:" + std::to_string(group_index),
                    .is_directory = true,
                    .is_current = group_index == this->iptv_overlay_group_index,
                    .smb_relative_path = {},
                    .webdav_relative_path = {},
                    .iptv_group_index = group_index,
                    .iptv_entry_index = std::numeric_limits<size_t>::max(),
                });
            }
        } else {
            const auto entry_indices = current_iptv_overlay_entry_indices();
            this->overlay_entries.reserve(entry_indices.size());
            for (const size_t entry_index : entry_indices) {
                if (entry_index >= this->iptv_overlay_context->entries.size()) {
                    continue;
                }

                const auto& entry = this->iptv_overlay_context->entries[entry_index];
                this->overlay_entries.push_back({
                    .title = entry.title.empty() ? tr("player_page/fallback_title") : entry.title,
                    .stable_key = entry.favorite_key.empty() ? entry.stream_url : entry.favorite_key,
                    .is_directory = false,
                    .is_current =
                        !this->target.iptv_overlay_entry_key.empty() && entry.favorite_key == this->target.iptv_overlay_entry_key,
                    .smb_relative_path = {},
                    .webdav_relative_path = {},
                    .iptv_group_index = std::numeric_limits<size_t>::max(),
                    .iptv_entry_index = entry_index,
                });
            }
        }
    }

    if (this->overlay_entries.empty()) {
        this->overlay_selected_index = -1;
        sync_overlay_to_surface();
        return;
    }

    int matched_index = -1;
    if (!selected_key.empty()) {
        for (int index = 0; index < static_cast<int>(this->overlay_entries.size()); ++index) {
            if (this->overlay_entries[static_cast<size_t>(index)].stable_key == selected_key) {
                matched_index = index;
                break;
            }
        }
    }

    if (matched_index < 0) {
        for (int index = 0; index < static_cast<int>(this->overlay_entries.size()); ++index) {
            if (this->overlay_entries[static_cast<size_t>(index)].is_current) {
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
    if (this->has_smb_source) {
        model.path = switchbox::core::smb_display_path(this->smb_source, this->overlay_relative_path);
    } else if (this->has_webdav_source) {
        model.path = switchbox::core::webdav_display_path(this->webdav_source, this->overlay_relative_path);
    } else if (this->iptv_overlay_context != nullptr) {
        model.path = this->target.source_label;
        if (!this->iptv_overlay_group_picker) {
            const std::string group_title = current_iptv_overlay_group_title();
            if (!group_title.empty()) {
                model.path += " / ";
                model.path += group_title;
            }
        }
    } else {
        model.path = this->target.source_label;
    }
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
    model.loading_overlay_title = startup_loading_title_for_source_kind(this->target.source_kind);
    model.loading_overlay_message = this->startup_loading_message;
    model.loading_overlay_detail = this->startup_loading_detail;
    model.loading_overlay_progress = std::clamp(this->startup_loading_progress, 0.0f, 1.0f);
    model.overlay_marquee_delay_ms = general.overlay_marquee_delay_ms;
    model.touch_enable = general.touch_enable;
    model.touch_player_gestures = general.touch_player_gestures;
    model.volume_osd_visible = this->volume_osd_visible && !this->controls_visible && !this->overlay_visible;
    model.volume_osd_value = std::clamp(this->session_volume, 0, 100);
    model.entries.reserve(this->overlay_entries.size());
    for (const auto& entry : this->overlay_entries) {
        model.entries.push_back({
            .title = entry.title,
            .is_directory = entry.is_directory,
            .is_current = entry.is_current,
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
    if (this->has_smb_source && entry.is_directory) {
        this->overlay_relative_path = entry.smb_relative_path;
        refresh_overlay_entries(false);
        return;
    }

    if (this->has_smb_source) {
        start_playback_with_target(
            switchbox::core::make_smb_playback_target(this->smb_source, entry.smb_relative_path));
        return;
    }

    if (this->has_webdav_source && entry.is_directory) {
        this->overlay_relative_path = entry.webdav_relative_path;
        refresh_overlay_entries(false);
        return;
    }

    if (this->has_webdav_source) {
        start_playback_with_target(
            switchbox::core::make_webdav_playback_target(this->webdav_source, entry.webdav_relative_path));
        return;
    }

    if (this->iptv_overlay_context == nullptr) {
        return;
    }

    if (this->iptv_overlay_group_picker) {
        if (entry.iptv_group_index >= this->iptv_overlay_context->groups.size()) {
            return;
        }

        this->iptv_overlay_group_index = entry.iptv_group_index;
        this->iptv_overlay_group_picker = false;
        refresh_overlay_entries(false);
        return;
    }

    if (entry.iptv_entry_index >= this->iptv_overlay_context->entries.size()) {
        return;
    }

    start_playback_with_target(
        switchbox::core::make_iptv_playback_target(
            this->iptv_overlay_context->source,
            this->iptv_overlay_context->entries[entry.iptv_entry_index],
            this->iptv_overlay_context,
            this->iptv_overlay_group_index));
}

void PlayerActivity::overlay_go_parent() {
    if (!this->overlay_visible) {
        return;
    }

    if (this->has_smb_source) {
        if (this->overlay_relative_path.empty()) {
            this->overlay_visible = false;
            sync_overlay_to_surface();
            return;
        }

        this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->overlay_relative_path);
        refresh_overlay_entries(false);
        return;
    }

    if (this->has_webdav_source) {
        if (this->overlay_relative_path.empty()) {
            this->overlay_visible = false;
            sync_overlay_to_surface();
            return;
        }

        this->overlay_relative_path = switchbox::core::webdav_parent_relative_path(this->overlay_relative_path);
        refresh_overlay_entries(false);
        return;
    }

    if (this->iptv_overlay_context != nullptr) {
        if (!this->iptv_overlay_group_picker) {
            this->iptv_overlay_group_picker = true;
            refresh_overlay_entries(true);
        } else {
            this->overlay_visible = false;
            sync_overlay_to_surface();
        }
        return;
    }

    this->overlay_visible = false;
    sync_overlay_to_surface();
}

void PlayerActivity::toggle_overlay() {
    this->overlay_visible = !this->overlay_visible;
    if (this->overlay_visible) {
        this->volume_osd_visible = false;
        this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
        this->iptv_overlay_group_picker = false;
        if (this->has_smb_source) {
            this->overlay_relative_path = switchbox::core::smb_parent_relative_path(this->current_relative_path);
        } else if (this->has_webdav_source) {
            this->overlay_relative_path = switchbox::core::webdav_parent_relative_path(this->current_relative_path);
        }
    }
    if (this->overlay_visible) {
        refresh_overlay_entries(false);
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
    update_playback_history_state();
    maybe_apply_resume_restore();
    flush_playback_resume_state(false);
    update_auto_sleep_state();
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

void PlayerActivity::update_playback_history_state() {
    if (this->playback_history_recorded_for_target) {
        return;
    }

    const std::uint64_t restart_at_ms = switchbox::core::switch_mpv_get_playback_restart_at_ms();
    if (restart_at_ms == 0 || restart_at_ms == this->playback_history_restart_at_ms) {
        return;
    }

    this->playback_history_restart_at_ms = restart_at_ms;
    this->playback_history_recorded_for_target = true;
    (void)switchbox::core::record_playback_history_for_target(
        switchbox::core::AppConfigStore::paths(),
        switchbox::core::AppConfigStore::current(),
        this->target);
}

void PlayerActivity::refresh_resume_restore_state_for_target() {
    double resume_position_seconds = 0.0;
    double resume_duration_seconds = 0.0;
    if (!switchbox::core::load_playback_resume_for_target(
            switchbox::core::AppConfigStore::paths(),
            switchbox::core::AppConfigStore::current(),
            this->target,
            resume_position_seconds,
            resume_duration_seconds)) {
        return;
    }

    if (resume_position_seconds <= 0.0 || resume_duration_seconds <= 0.0) {
        return;
    }

    this->resume_restore_pending = true;
    this->resume_restore_position_seconds = resume_position_seconds;
    this->resume_restore_started_at = std::chrono::steady_clock::now();
    switchbox::core::switch_mpv_append_debug_log_note(
        "resume_restore pending position_seconds=" + std::to_string(resume_position_seconds));
}

void PlayerActivity::maybe_apply_resume_restore() {
    if (!this->resume_restore_pending ||
        this->playback_error_dialog_open ||
        this->startup_dialog_pending ||
        this->pending_startup_task != nullptr) {
        return;
    }

    if (!switchbox::core::switch_mpv_session_active() || !switchbox::core::switch_mpv_has_media()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (this->resume_restore_started_at == std::chrono::steady_clock::time_point::min()) {
        this->resume_restore_started_at = now;
    } else if (now >= this->resume_restore_started_at + std::chrono::milliseconds(kResumeRestoreTimeoutMs)) {
        this->resume_restore_pending = false;
        this->resume_restore_position_seconds = 0.0;
        switchbox::core::switch_mpv_append_debug_log_note("resume_restore timeout");
        return;
    }

    if (switchbox::core::switch_mpv_get_playback_restart_at_ms() == 0) {
        return;
    }

    const double duration = switchbox::core::switch_mpv_get_duration_seconds();
    if (duration <= 0.0 || !switchbox::core::switch_mpv_is_seekable()) {
        return;
    }

    const double stop_percent = std::clamp(
        static_cast<double>(switchbox::core::AppConfigStore::current().general.resume_stop_percent),
        0.0,
        100.0);
    const double clamped_target_seconds =
        std::clamp(this->resume_restore_position_seconds, 0.0, duration);
    const double remaining_percent =
        duration > 0.0 ? std::max(0.0, duration - clamped_target_seconds) * 100.0 / duration : 100.0;
    if (clamped_target_seconds <= 0.0 || remaining_percent <= stop_percent) {
        this->resume_restore_pending = false;
        this->resume_restore_position_seconds = 0.0;
        return;
    }

    const double current_position = switchbox::core::switch_mpv_get_position_seconds();
    if (current_position > 0.0 && std::fabs(current_position - clamped_target_seconds) <= 1.0) {
        this->resume_restore_pending = false;
        this->resume_restore_position_seconds = 0.0;
        return;
    }

    if (seek_absolute(clamped_target_seconds)) {
        this->resume_restore_pending = false;
        this->resume_restore_position_seconds = 0.0;
        this->last_resume_persist_time = now;
        switchbox::core::switch_mpv_append_debug_log_note(
            "resume_restore applied position_seconds=" + std::to_string(clamped_target_seconds));
    }
}

void PlayerActivity::flush_playback_resume_state(bool force) {
    if (this->pending_startup_task != nullptr ||
        (!force && this->startup_dialog_pending) ||
        (!force && this->playback_error_dialog_open)) {
        return;
    }

    if (!switchbox::core::playback_target_uses_history(
            switchbox::core::AppConfigStore::current(),
            this->target)) {
        return;
    }

    if (!switchbox::core::switch_mpv_session_active() || !switchbox::core::switch_mpv_has_media()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force &&
        this->last_resume_persist_time != std::chrono::steady_clock::time_point::min() &&
        now < this->last_resume_persist_time + std::chrono::milliseconds(kResumePersistIntervalMs)) {
        return;
    }

    const double duration = switchbox::core::switch_mpv_get_duration_seconds();
    if (duration <= 0.0 || !switchbox::core::switch_mpv_is_seekable()) {
        return;
    }

    const double position = std::clamp(switchbox::core::switch_mpv_get_position_seconds(), 0.0, duration);
    if (position <= 0.0) {
        return;
    }

    const auto& general = switchbox::core::AppConfigStore::current().general;
    const double start_percent = std::clamp(static_cast<double>(general.resume_start_percent), 0.0, 100.0);
    const double stop_percent = std::clamp(static_cast<double>(general.resume_stop_percent), 0.0, 100.0);
    const double progress_percent = duration > 0.0 ? position * 100.0 / duration : 0.0;
    const double remaining_percent = duration > 0.0 ? std::max(0.0, duration - position) * 100.0 / duration : 100.0;

    if (progress_percent < start_percent) {
        return;
    }

    if (remaining_percent <= stop_percent) {
        (void)switchbox::core::clear_playback_resume_for_target(
            switchbox::core::AppConfigStore::paths(),
            switchbox::core::AppConfigStore::current(),
            this->target);
        this->last_resume_persist_time = now;
        return;
    }

    if (switchbox::core::update_playback_resume_for_target(
            switchbox::core::AppConfigStore::paths(),
            switchbox::core::AppConfigStore::current(),
            this->target,
            position,
            duration)) {
        this->last_resume_persist_time = now;
    }
}

void PlayerActivity::update_auto_sleep_state() {
#if defined(__SWITCH__)
    const bool should_disable_idle_behaviors =
        switchbox::core::switch_mpv_session_active() &&
        switchbox::core::switch_mpv_has_media() &&
        !switchbox::core::switch_mpv_is_paused();

    if (should_disable_idle_behaviors != this->media_playback_state_enabled) {
        if (R_SUCCEEDED(appletSetMediaPlaybackState(should_disable_idle_behaviors))) {
            this->media_playback_state_enabled = should_disable_idle_behaviors;
        }
    }

    if (should_disable_idle_behaviors != this->auto_sleep_disabled) {
        if (R_SUCCEEDED(appletSetAutoSleepDisabled(should_disable_idle_behaviors))) {
            this->auto_sleep_disabled = should_disable_idle_behaviors;
        }
    }
#endif
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
        this->last_vertical_repeat_time = now + std::chrono::milliseconds(this->button_long_press_threshold_ms);
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
            now + std::chrono::milliseconds(this->button_long_press_threshold_ms);
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
    const auto& controller = brls::Application::getControllerState();
    const bool r_pressed = controller.buttons[brls::BUTTON_RB];
    const bool l_pressed = controller.buttons[brls::BUTTON_LB];
    const bool zr_pressed = controller.buttons[brls::BUTTON_RT];
    const bool zl_pressed = controller.buttons[brls::BUTTON_LT];
    const bool left_pressed = direction_left_pressed(controller);
    const bool right_pressed = direction_right_pressed(controller);

    int mode = 0;
    bool mode_is_combo = false;
    if (!this->overlay_visible && zr_pressed && left_pressed) {
        mode = -2;
        mode_is_combo = true;
    } else if (!this->overlay_visible && zr_pressed && right_pressed) {
        mode = 2;
        mode_is_combo = true;
    } else if (!this->overlay_visible && r_pressed && left_pressed) {
        mode = -1;
        mode_is_combo = true;
    } else if (!this->overlay_visible && r_pressed && right_pressed) {
        mode = 1;
        mode_is_combo = true;
    } else {
        const int shoulder_count =
            static_cast<int>(l_pressed) +
            static_cast<int>(r_pressed) +
            static_cast<int>(zl_pressed) +
            static_cast<int>(zr_pressed);
        if (shoulder_count == 1) {
            if (l_pressed) {
                mode = -4;
            } else if (r_pressed) {
                mode = 4;
            } else if (zl_pressed) {
                mode = -5;
            } else if (zr_pressed) {
                mode = 5;
            }
        }
    }

    if (mode == 0) {
        this->last_continuous_seek_mode = 0;
        this->last_continuous_seek_time = std::chrono::steady_clock::time_point::min();
        return;
    }

    const int interval_ms = std::max(10, switchbox::core::AppConfigStore::current().general.continuous_seek_interval_ms);
    const auto now = std::chrono::steady_clock::now();
    if (mode != this->last_continuous_seek_mode) {
        this->last_continuous_seek_mode = mode;
        this->last_continuous_seek_time = mode_is_combo
                                              ? std::chrono::steady_clock::time_point::min()
                                              : now + std::chrono::milliseconds(this->button_long_press_threshold_ms);
        if (!mode_is_combo) {
            return;
        }
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
        case -4:
            seek_relative(-short_seek);
            break;
        case 4:
            seek_relative(short_seek);
            break;
        case -5:
            seek_relative(-long_seek);
            break;
        case 5:
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
    const bool y_down = controller.buttons[brls::BUTTON_Y];
    const auto now = std::chrono::steady_clock::now();

    if (y_down && !this->y_button_down) {
        this->y_button_down = true;
        this->y_hold_speed_active = false;
        this->y_button_pressed_at = now;
    } else if (!y_down && this->y_button_down) {
        const bool was_hold = this->y_hold_speed_active;
        this->y_button_down = false;
        this->y_hold_speed_active = false;
        this->y_button_pressed_at = std::chrono::steady_clock::time_point::min();
        if (!was_hold) {
            this->sticky_speed_enabled = !this->sticky_speed_enabled;
        }
    } else if (y_down &&
               !this->y_hold_speed_active &&
               this->y_button_pressed_at != std::chrono::steady_clock::time_point::min() &&
               now >= this->y_button_pressed_at + std::chrono::milliseconds(this->button_long_press_threshold_ms)) {
        this->y_hold_speed_active = true;
    }

    const double configured_speed =
        std::max(0.1, static_cast<double>(switchbox::core::AppConfigStore::current().general.y_hold_speed_multiplier));
    double target_speed = 1.0;
    if (this->sticky_speed_enabled) {
        target_speed = configured_speed;
    }
    if (this->y_hold_speed_active) {
        target_speed = this->sticky_speed_enabled ? (target_speed + configured_speed) : configured_speed;
    }
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
    this->startup_loading_detail.clear();
    this->startup_loading_progress = 0.0f;
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

void PlayerActivity::stop_playback_session_before_leave(bool persist_resume) {
    if (this->playback_session_stopped) {
        return;
    }

    switchbox::core::switch_mpv_append_debug_log_note("stop_before_leave begin");
    this->playback_session_stopped = true;
    if (persist_resume) {
        flush_playback_resume_state(true);
    }
    cancel_pending_startup_task();
    if (this->runtime_initialized) {
        this->runtime_tick.stop();
    }

    this->startup_loading_active = false;
    this->startup_loading_overlay_visible = false;
    this->startup_loading_message.clear();
    this->startup_loading_detail.clear();
    this->startup_loading_progress = 0.0f;
    this->startup_loading_started_at = std::chrono::steady_clock::time_point::min();
    this->controls_visible = false;
    this->overlay_visible = false;
    this->volume_osd_visible = false;
    this->volume_osd_hide_time = std::chrono::steady_clock::time_point::min();
    sync_overlay_to_surface();
#if defined(__SWITCH__)
    if (this->media_playback_state_enabled) {
        appletSetMediaPlaybackState(false);
        this->media_playback_state_enabled = false;
    }
    if (this->auto_sleep_disabled) {
        appletSetAutoSleepDisabled(false);
        this->auto_sleep_disabled = false;
    }
#endif
    switchbox::core::switch_mpv_stop();
    switchbox::core::switch_mpv_append_debug_log_note("stop_before_leave end");
}

std::vector<size_t> PlayerActivity::current_iptv_overlay_entry_indices() const {
    if (this->iptv_overlay_context == nullptr || this->iptv_overlay_context->groups.empty()) {
        return {};
    }

    const size_t group_index = std::min(this->iptv_overlay_group_index, this->iptv_overlay_context->groups.size() - 1);
    const auto& group = this->iptv_overlay_context->groups[group_index];
    if (!group.favorites) {
        return group.entry_indices;
    }

    std::unordered_set<std::string> favorite_keys(
        this->iptv_overlay_context->source.favorite_keys.begin(),
        this->iptv_overlay_context->source.favorite_keys.end());
    std::unordered_set<std::string> emitted_keys;
    std::vector<size_t> indices;
    indices.reserve(this->iptv_overlay_context->entries.size());

    for (size_t index = 0; index < this->iptv_overlay_context->entries.size(); ++index) {
        const auto& favorite_key = this->iptv_overlay_context->entries[index].favorite_key;
        if (favorite_keys.contains(favorite_key) && emitted_keys.emplace(favorite_key).second) {
            indices.push_back(index);
        }
    }

    return indices;
}

std::string PlayerActivity::current_iptv_overlay_group_title() const {
    if (this->iptv_overlay_context == nullptr || this->iptv_overlay_context->groups.empty()) {
        return {};
    }

    const size_t group_index = std::min(this->iptv_overlay_group_index, this->iptv_overlay_context->groups.size() - 1);
    return this->iptv_overlay_context->groups[group_index].title;
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

void PlayerActivity::handle_touch_double_tap() {
    const auto& general = switchbox::core::AppConfigStore::current().general;
    if (!general.touch_enable || !general.touch_player_gestures) {
        return;
    }

    if (this->playback_error_dialog_open || this->startup_dialog_pending) {
        return;
    }

    if (!switchbox::core::switch_mpv_session_active() && !switchbox::core::switch_mpv_has_media()) {
        return;
    }

    if (switchbox::core::switch_mpv_is_paused()) {
        return;
    }

    switchbox::core::switch_mpv_toggle_pause();
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
    if ((!this->has_smb_source && !this->has_webdav_source) || this->current_relative_path.empty()) {
        brls::Application::notify("Current source is not deletable.");
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
        const std::string deleting_relative_path = this->current_relative_path;
        const std::string next_focus = find_next_focus_after_delete();
        stop_playback_session_before_leave(false);
        if (this->has_smb_source) {
            SmbBrowserActivity::request_delete_after_return(
                this->smb_source,
                switchbox::core::smb_parent_relative_path(this->current_relative_path),
                next_focus,
                deleting_relative_path);
        } else if (this->has_webdav_source) {
            WebDavBrowserActivity::request_delete_after_return(
                this->webdav_source,
                switchbox::core::webdav_parent_relative_path(this->current_relative_path),
                next_focus,
                deleting_relative_path);
        }
        dismiss_to_previous_activity_if_still_top();
    });
    dialog->open();
}

std::string PlayerActivity::find_next_focus_after_delete() const {
    if (this->current_relative_path.empty()) {
        return {};
    }

    const auto& general = switchbox::core::AppConfigStore::current().general;
    std::vector<std::string> files;

    if (this->has_smb_source) {
        const std::string directory = switchbox::core::smb_parent_relative_path(this->current_relative_path);
        const auto result = switchbox::core::browse_smb_directory(
            this->smb_source,
            general,
            directory);
        if (!result.success || result.entries.empty()) {
            return {};
        }

        files.reserve(result.entries.size());
        for (const auto& entry : result.entries) {
            if (!entry.is_directory) {
                files.push_back(entry.relative_path);
            }
        }
    } else if (this->has_webdav_source) {
        const std::string directory = switchbox::core::webdav_parent_relative_path(this->current_relative_path);
        const auto result = switchbox::core::browse_webdav_directory(
            this->webdav_source,
            general,
            directory);
        if (!result.success || result.entries.empty()) {
            return {};
        }

        files.reserve(result.entries.size());
        for (const auto& entry : result.entries) {
            if (!entry.is_directory) {
                files.push_back(entry.relative_path);
            }
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
    source.key = this->target.source_key.empty() ? "runtime" : this->target.source_key;
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

switchbox::core::WebDavSourceSettings PlayerActivity::make_webdav_source_from_target() const {
    switchbox::core::WebDavSourceSettings source;
    source.key = this->target.source_key.empty() ? "runtime" : this->target.source_key;
    source.title = this->target.source_label.empty() ? "WebDAV" : this->target.source_label;
    if (this->target.webdav_locator.has_value()) {
        source.url = this->target.webdav_locator->url;
        source.username = this->target.webdav_locator->username;
        source.password = this->target.webdav_locator->password;
    }
    return source;
}

}  // namespace switchbox::app
