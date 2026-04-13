#include "switchbox/app/home_activity.hpp"

#include <utility>

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/placeholder_activity.hpp"
#include "switchbox/core/build_info.hpp"

namespace switchbox::app {

namespace {

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
        .title = "IPTV",
        .subtitle = "Live channels, playlist intake and grouped navigation",
        .checkpoints =
            {
                "Add source profiles, sample playlists and refresh flow",
                "Build grouped channel navigation with metadata rows",
                "Pass live channel selection into the shared player activity",
            },
    };
}

PlaceholderSection make_smb_section() {
    return {
        .title = "SMB / NAS",
        .subtitle = "Saved shares, browsing flow and seekable media access",
        .checkpoints =
            {
                "Add saved servers, credentials and share targets",
                "Validate directory listing, probing and seekable reads",
                "Pass selected files into the shared player activity",
            },
    };
}

PlaceholderSection make_player_section() {
    return {
        .title = "Playback Test",
        .subtitle = "Shared player shell, transport controls and runtime state",
        .checkpoints =
            {
                "Define player layout, overlays and playback state model",
                "Add fixed desktop and Switch media entry points",
                "Wire buffering, transport controls and exit flow",
            },
    };
}

PlaceholderSection make_settings_section() {
    return {
        .title = "Settings",
        .subtitle = "Runtime checks, source config and diagnostics surface",
        .checkpoints =
            {
                "Surface runtime target, build info and health checks",
                "Reserve IPTV and SMB configuration sections",
                "Prepare logging, storage and environment validation",
            },
    };
}

brls::DetailCell* create_nav_cell(
    const std::string& title,
    const std::string& detail,
    PlaceholderSection section) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->registerClickAction(
        [section = std::move(section)](brls::View*) {
            brls::Application::pushActivity(new PlaceholderActivity(section));
            return true;
        });
    return cell;
}

void apply_native_status_layout(brls::AppletFrame* frame) {
    frame->setTitle("SwitchBOX");

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

    auto* eyebrow = create_label("MEDIA CONTROL SURFACE", 15.0f, theme["brls/highlight/color2"], true);
    eyebrow->setMargins(0, 0, 8, 0);
    container->addView(eyebrow);

    auto* header = new brls::Header();
    header->setTitle("SwitchBOX");
    header->setSubtitle("Switch-first launcher for live, local and test playback");
    container->addView(header);

    auto* summary = create_label(
        "Bootstrap is complete. This screen now acts as the product hub that routes every media source into one shared playback experience.",
        19.0f,
        theme["brls/text"]);
    summary->setMargins(0, 0, 14, 0);
    container->addView(summary);

    container->addView(create_info_cell(
        "Build health",
        "Desktop and Switch builds are green, including .nro output",
        theme["brls/highlight/color2"]));
    container->addView(create_info_cell(
        "Current focus",
        "Promote Playback Test into the first real player screen",
        theme["brls/list/listItem_value_color"]));
    container->addView(create_info_cell(
        "Primary promise",
        "Every source should land in one consistent playback shell",
        theme["brls/text"]));
    container->addView(create_info_cell(
        "Running on",
        context.platform_name,
        theme["brls/text"]));
    container->addView(create_info_cell(
        "Runtime mode",
        context.switch_target ? "Switch app target" : "Desktop debug target",
        theme["brls/text"]));
    container->addView(create_info_cell(
        "Build version",
        switchbox::core::BuildInfo::version_string(),
        theme["brls/text"]));

    auto* sectionLabel = create_label("Launch modules", 24.0f, theme["brls/text"], true);
    sectionLabel->setMargins(14, 0, 4, 0);
    container->addView(sectionLabel);

    auto* sectionSummary = create_label(
        "Playback is the first delivery target. IPTV and SMB are upstream entry flows that should converge into that same player.",
        17.0f,
        theme["brls/header/subtitle"]);
    sectionSummary->setMargins(0, 0, 10, 0);
    container->addView(sectionSummary);

    container->addView(create_nav_cell(
        "Playback Test",
        "Build the shared player shell, transport controls and test media flow",
        make_player_section()));

    container->addView(create_nav_cell(
        "IPTV",
        "Add playlists, channel groups and live-entry handoff into the player",
        make_iptv_section()));

    container->addView(create_nav_cell(
        "SMB / NAS",
        "Add saved shares, file browsing and seekable handoff into the player",
        make_smb_section()));

    auto* noteLabel = create_label("Delivery note", 24.0f, theme["brls/text"], true);
    noteLabel->setMargins(14, 0, 4, 0);
    container->addView(noteLabel);

    auto* noteSummary = create_label(
        "The launcher can stay simple. The value of this project comes from stable playback, clean source handoff and reliable runtime diagnostics.",
        17.0f,
        theme["brls/header/subtitle"]);
    noteSummary->setMargins(0, 0, 10, 0);
    container->addView(noteSummary);

    auto* systemLabel = create_label("System", 24.0f, theme["brls/text"], true);
    systemLabel->setMargins(14, 0, 4, 0);
    container->addView(systemLabel);

    auto* systemSummary = create_label(
        "Diagnostics and configuration stay here so the media entry modules can remain focused and predictable.",
        17.0f,
        theme["brls/header/subtitle"]);
    systemSummary->setMargins(0, 0, 10, 0);
    container->addView(systemSummary);

    container->addView(create_nav_cell(
        "Settings",
        "Runtime checks, source configuration, diagnostics and build metadata",
        make_settings_section()));

    auto* scrollingFrame = new brls::ScrollingFrame();
    scrollingFrame->setContentView(container);
    scrollingFrame->getAppletFrameItem()->setHintView(new HeaderStatusHint());

    auto* frame = new brls::AppletFrame(scrollingFrame);
    frame->registerAction(
        "设置",
        brls::BUTTON_START,
        [](brls::View*) {
            brls::Application::pushActivity(new PlaceholderActivity(make_settings_section()));
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
