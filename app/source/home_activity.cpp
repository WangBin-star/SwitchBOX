#include "switchbox/app/home_activity.hpp"

#include <functional>
#include <utility>

#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/placeholder_activity.hpp"
#include "switchbox/app/settings_activity.hpp"
#include "switchbox/core/build_info.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

brls::Label* create_label(
    const std::string& text,
    float fontSize,
    NVGcolor color,
    bool singleLine = false) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(fontSize);
    label->setTextColor(color);
    label->setSingleLine(singleLine);
    return label;
}

brls::DetailCell* create_info_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detailColor) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->setDetailTextColor(detailColor);
    return cell;
}

PlaceholderSection make_iptv_section() {
    return {
        .title = tr("sections/iptv/title"),
        .subtitle = tr("sections/iptv/subtitle"),
        .checkpoints =
            {
                tr("sections/iptv/checkpoints/1"),
                tr("sections/iptv/checkpoints/2"),
                tr("sections/iptv/checkpoints/3"),
            },
    };
}

PlaceholderSection make_smb_section() {
    return {
        .title = tr("sections/smb/title"),
        .subtitle = tr("sections/smb/subtitle"),
        .checkpoints =
            {
                tr("sections/smb/checkpoints/1"),
                tr("sections/smb/checkpoints/2"),
                tr("sections/smb/checkpoints/3"),
            },
    };
}

PlaceholderSection make_player_section() {
    return {
        .title = tr("sections/player/title"),
        .subtitle = tr("sections/player/subtitle"),
        .checkpoints =
            {
                tr("sections/player/checkpoints/1"),
                tr("sections/player/checkpoints/2"),
                tr("sections/player/checkpoints/3"),
            },
    };
}

brls::DetailCell* create_action_cell(
    const std::string& title,
    const std::string& detail,
    std::function<void()> action) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->registerClickAction(
        [action = std::move(action)](brls::View*) {
            action();
            return true;
        });
    return cell;
}

brls::DetailCell* create_nav_cell(
    const std::string& title,
    const std::string& detail,
    PlaceholderSection section) {
    return create_action_cell(
        title,
        detail,
        [section = std::move(section)]() {
            brls::Application::pushActivity(new PlaceholderActivity(section));
        });
}

void apply_native_status_layout(brls::AppletFrame* frame) {
    frame->setTitle(tr("brand/app_name"));

    if (auto* timeView = frame->getView("brls/hints/time")) {
        timeView->setVisibility(brls::Visibility::GONE);
    }

    if (auto* wirelessView = frame->getView("brls/wireless")) {
        wirelessView->setVisibility(brls::Visibility::GONE);
    }

    if (auto* batteryView = frame->getView("brls/battery")) {
        batteryView->setVisibility(brls::Visibility::GONE);
    }
}

brls::View* create_home_content(const StartupContext& context) {
    auto theme = brls::Application::getTheme();

    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(30, 40, 30, 40);

    auto* eyebrow = create_label(tr("home/eyebrow"), 15.0f, theme["brls/highlight/color2"], true);
    eyebrow->setMargins(0, 0, 8, 0);
    container->addView(eyebrow);

    auto* header = new brls::Header();
    header->setTitle(tr("brand/app_name"));
    header->setSubtitle(tr("home/header_subtitle"));
    container->addView(header);

    auto* summary = create_label(
        tr("home/summary"),
        19.0f,
        theme["brls/text"]);
    summary->setMargins(0, 0, 14, 0);
    container->addView(summary);

    container->addView(create_info_cell(
        tr("home/info/build_health/title"),
        tr("home/info/build_health/detail"),
        theme["brls/highlight/color2"]));
    container->addView(create_info_cell(
        tr("home/info/current_focus/title"),
        tr("home/info/current_focus/detail"),
        theme["brls/list/listItem_value_color"]));
    container->addView(create_info_cell(
        tr("home/info/primary_promise/title"),
        tr("home/info/primary_promise/detail"),
        theme["brls/text"]));
    container->addView(create_info_cell(
        tr("home/info/running_on/title"),
        context.platform_name,
        theme["brls/text"]));
    container->addView(create_info_cell(
        tr("home/info/runtime_mode/title"),
        context.switch_target ? tr("home/info/runtime_mode/switch") : tr("home/info/runtime_mode/desktop"),
        theme["brls/text"]));
    container->addView(create_info_cell(
        tr("home/info/build_version/title"),
        switchbox::core::BuildInfo::version_string(),
        theme["brls/text"]));

    auto* sectionLabel = create_label(tr("home/modules/title"), 24.0f, theme["brls/text"], true);
    sectionLabel->setMargins(14, 0, 4, 0);
    container->addView(sectionLabel);

    auto* sectionSummary = create_label(
        tr("home/modules/summary"),
        17.0f,
        theme["brls/header/subtitle"]);
    sectionSummary->setMargins(0, 0, 10, 0);
    container->addView(sectionSummary);

    container->addView(create_nav_cell(
        tr("sections/player/title"),
        tr("home/modules/player_detail"),
        make_player_section()));

    container->addView(create_nav_cell(
        tr("sections/iptv/title"),
        tr("home/modules/iptv_detail"),
        make_iptv_section()));

    container->addView(create_nav_cell(
        tr("sections/smb/title"),
        tr("home/modules/smb_detail"),
        make_smb_section()));

    auto* noteLabel = create_label(tr("home/note/title"), 24.0f, theme["brls/text"], true);
    noteLabel->setMargins(14, 0, 4, 0);
    container->addView(noteLabel);

    auto* noteSummary = create_label(
        tr("home/note/summary"),
        17.0f,
        theme["brls/header/subtitle"]);
    noteSummary->setMargins(0, 0, 10, 0);
    container->addView(noteSummary);

    auto* systemLabel = create_label(tr("home/system/title"), 24.0f, theme["brls/text"], true);
    systemLabel->setMargins(14, 0, 4, 0);
    container->addView(systemLabel);

    auto* systemSummary = create_label(
        tr("home/system/summary"),
        17.0f,
        theme["brls/header/subtitle"]);
    systemSummary->setMargins(0, 0, 10, 0);
    container->addView(systemSummary);

    container->addView(create_action_cell(
        tr("sections/settings/title"),
        tr("home/system/settings_detail"),
        []() {
            brls::Application::pushActivity(new SettingsActivity());
        }));

    auto* scrollingFrame = new brls::ScrollingFrame();
    scrollingFrame->setContentView(container);
    scrollingFrame->getAppletFrameItem()->setHintView(new HeaderStatusHint());

    auto* frame = new brls::AppletFrame(scrollingFrame);
    frame->registerAction(
        tr("actions/settings"),
        brls::BUTTON_START,
        [](brls::View*) {
            brls::Application::pushActivity(new SettingsActivity());
            return true;
        },
        false,
        false,
        brls::SOUND_CLICK);
    apply_native_status_layout(frame);
    return frame;
}

}  // namespace

HomeActivity::HomeActivity(const StartupContext& context)
    : brls::Activity(create_home_content(context))
    , context(context) {
}

}  // namespace switchbox::app
