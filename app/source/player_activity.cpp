#include "switchbox/app/player_activity.hpp"

#include <utility>

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/core/box.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/hint.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/player_video_surface.hpp"
#include "switchbox/core/playback_launcher.hpp"
#include "switchbox/core/switch_mpv_player.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

bool can_attempt_launch(const switchbox::core::PlaybackTarget& target) {
    return !target.primary_locator.empty() || !target.fallback_locator.empty();
}

#if !defined(__SWITCH__)
brls::Label* create_label(
    const std::string& text,
    float font_size,
    NVGcolor color,
    bool single_line = false) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(font_size);
    label->setTextColor(color);
    label->setSingleLine(single_line);
    return label;
}

brls::DetailCell* create_info_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detail_color) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->setDetailTextColor(detail_color);
    return cell;
}

void apply_native_status_layout(brls::AppletFrame* frame, const std::string& title) {
    frame->setTitle(title);

    if (auto* content = frame->getContentView()) {
        content->getAppletFrameItem()->setHintView(new HeaderStatusHint());
    }

    if (auto* time_view = frame->getView("brls/hints/time")) {
        time_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* wireless_view = frame->getView("brls/wireless")) {
        wireless_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* battery_view = frame->getView("brls/battery")) {
        battery_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* hints = dynamic_cast<brls::Hints*>(frame->getView("brls/hints"))) {
        hints->setAllowAButtonTouch(true);
    }
}

std::string initial_status_detail(const switchbox::core::PlaybackTarget& target) {
    if (!can_attempt_launch(target)) {
        return tr("player_page/status/missing_locator");
    }

    return tr("player_page/status/ready_desktop");
}

brls::View* create_desktop_player_content(const switchbox::core::PlaybackTarget& target) {
    auto theme = brls::Application::getTheme();

    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(30, 40, 30, 40);

    auto* eyebrow = create_label(
        tr("player_page/eyebrow"),
        15.0f,
        theme["brls/highlight/color2"],
        true);
    eyebrow->setMargins(0, 0, 8, 0);
    container->addView(eyebrow);

    auto* header = new brls::Header();
    header->setTitle(target.title.empty() ? tr("player_page/fallback_title") : target.title);
    header->setSubtitle(target.subtitle.empty() ? tr("player_page/fallback_subtitle") : target.subtitle);
    container->addView(header);

    auto* summary = create_label(
        tr("player_page/summary"),
        18.0f,
        theme["brls/text"]);
    summary->setMargins(0, 0, 12, 0);
    container->addView(summary);

    container->addView(create_info_cell(
        tr("player_page/status/title"),
        initial_status_detail(target),
        can_attempt_launch(target) ? theme["brls/highlight/color2"] : theme["brls/text_disabled"]));

    container->addView(create_info_cell(
        tr("player_page/source/title"),
        target.source_label.empty() ? tr("player_page/not_available") : target.source_label,
        theme["brls/list/listItem_value_color"]));

    container->addView(create_info_cell(
        tr("player_page/locator/title"),
        target.display_locator.empty() ? tr("player_page/not_available") : target.display_locator,
        theme["brls/list/listItem_value_color"]));

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(container);

    auto* frame = new brls::AppletFrame(scrolling_frame);
    apply_native_status_layout(frame, tr("player_page/title"));
    return frame;
}
#endif

#if defined(__SWITCH__)
brls::View* create_switch_player_content(const switchbox::core::PlaybackTarget&) {
    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(0, 0, 0, 0);
    container->setGrow(1.0f);
    container->setBackgroundColor(nvgRGB(0, 0, 0));
    auto* video_surface = new PlayerVideoSurface();
    video_surface->setId("switchbox/player_surface");
    video_surface->registerAction(
        tr("player_page/actions/toggle_pause"),
        brls::BUTTON_A,
        [](brls::View*) {
            switchbox::core::switch_mpv_toggle_pause();
            return true;
        },
        true,
        false,
        brls::SOUND_CLICK);
    video_surface->registerAction(
        brls::getStr("hints/back"),
        brls::BUTTON_B,
        [](brls::View*) {
            brls::Application::popActivity(brls::TransitionAnimation::FADE);
            return true;
        },
        true,
        false,
        brls::SOUND_CLICK);
    container->addView(video_surface);

    auto* frame = new brls::AppletFrame(container);
    frame->setHeaderVisibility(brls::Visibility::GONE);
    frame->setFooterVisibility(brls::Visibility::GONE);
    frame->setTitle("");
    return frame;
}
#endif

}  // namespace

PlayerActivity::PlayerActivity(switchbox::core::PlaybackTarget target)
#if defined(__SWITCH__)
    : brls::Activity(create_switch_player_content(target))
#else
    : brls::Activity(create_desktop_player_content(target))
#endif
    , target(std::move(target)) {
#if !defined(__SWITCH__)
    registerExitAction();
#endif

#if defined(__SWITCH__)
    std::string startup_error;
    if (!switchbox::core::switch_mpv_open(this->target, startup_error) && !startup_error.empty()) {
        auto* dialog = new brls::Dialog(startup_error);
        dialog->open();
    }

    registerAction(
        brls::getStr("hints/back"),
        brls::BUTTON_B,
        [](brls::View*) {
            brls::Application::popActivity(brls::TransitionAnimation::FADE);
            return true;
        });

    registerAction(
        tr("player_page/actions/toggle_pause"),
        brls::BUTTON_A,
        [this](brls::View*) {
            switchbox::core::switch_mpv_toggle_pause();
            return true;
        });

    if (auto* surface = this->getView("switchbox/player_surface")) {
        brls::Application::giveFocus(surface);
    }
#else
    registerAction(
        tr("player_page/actions/start"),
        brls::BUTTON_A,
        [this](brls::View* view) {
            return this->handle_start_action(view);
        });
#endif
}

PlayerActivity::~PlayerActivity() {
#if defined(__SWITCH__)
    switchbox::core::switch_mpv_stop();
#endif
}

bool PlayerActivity::handle_start_action(brls::View*) {
    if (!can_attempt_launch(this->target)) {
        auto* dialog = new brls::Dialog(tr("player_page/status/missing_locator"));
        dialog->open();
        return true;
    }

    const auto launch_result = switchbox::core::launch_playback(this->target);
    std::string message;

    if (launch_result.started) {
        message = launch_result.used_fallback_locator
                      ? tr("player_page/status/started_fallback")
                      : tr("player_page/status/started_primary");
    } else if (!launch_result.backend_available) {
        message = tr("player_page/status/backend_missing_platform");
    } else {
        message = tr("player_page/status/start_failed");
    }

    if (!launch_result.error_message.empty()) {
        message += "\n\n";
        message += launch_result.error_message;
    }

    auto* dialog = new brls::Dialog(message);
    dialog->open();
    return true;
}

}  // namespace switchbox::app
