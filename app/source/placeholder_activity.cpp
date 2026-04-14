#include "switchbox/app/placeholder_activity.hpp"

#include <utility>

#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"

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

brls::DetailCell* create_checkpoint_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detailColor) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->setDetailTextColor(detailColor);
    return cell;
}

void apply_native_status_layout(brls::AppletFrame* frame, const std::string& title) {
    frame->setTitle(title);

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

brls::View* create_placeholder_content(const PlaceholderSection& section) {
    auto theme = brls::Application::getTheme();
    const std::string firstCheckpoint =
        section.checkpoints.empty() ? tr("placeholder/fallbacks/first_task") : section.checkpoints.front();
    const std::string finalCheckpoint =
        section.checkpoints.empty() ? tr("placeholder/fallbacks/ship_slice") : section.checkpoints.back();

    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(30, 40, 30, 40);

    auto* eyebrow = create_label(tr("placeholder/eyebrow"), 15.0f, theme["brls/highlight/color2"], true);
    eyebrow->setMargins(0, 0, 8, 0);
    container->addView(eyebrow);

    auto* header = new brls::Header();
    header->setTitle(section.title);
    header->setSubtitle(section.subtitle);
    container->addView(header);

    auto* summary = create_label(
        tr("placeholder/summary"),
        18.0f,
        theme["brls/text"]);
    summary->setMargins(0, 0, 12, 0);
    container->addView(summary);

    container->addView(create_checkpoint_cell(
        tr("placeholder/current_state/title"),
        tr("placeholder/current_state/detail"),
        theme["brls/highlight/color2"]));
    container->addView(create_checkpoint_cell(
        tr("placeholder/first_deliverable/title"),
        firstCheckpoint,
        theme["brls/list/listItem_value_color"]));
    container->addView(create_checkpoint_cell(
        tr("placeholder/exit_condition/title"),
        finalCheckpoint,
        theme["brls/text"]));

    auto* checklistLabel = create_label(tr("placeholder/path/title"), 22.0f, theme["brls/text"], true);
    checklistLabel->setMargins(10, 0, 4, 0);
    container->addView(checklistLabel);

    auto* checklistSummary = create_label(
        tr("placeholder/path/summary"),
        16.0f,
        theme["brls/header/subtitle"]);
    checklistSummary->setMargins(0, 0, 10, 0);
    container->addView(checklistSummary);

    for (size_t index = 0; index < section.checkpoints.size(); index++) {
        auto* checkpointCell = create_checkpoint_cell(
            brls::getStr("switchbox/placeholder/path/step_label", index + 1),
            section.checkpoints[index],
            theme["brls/list/listItem_value_color"]);
        container->addView(checkpointCell);
    }

    auto* scrollingFrame = new brls::ScrollingFrame();
    scrollingFrame->setContentView(container);
    scrollingFrame->getAppletFrameItem()->setHintView(new HeaderStatusHint());

    auto* frame = new brls::AppletFrame(scrollingFrame);
    apply_native_status_layout(frame, section.title);
    return frame;
}

}  // namespace

PlaceholderActivity::PlaceholderActivity(PlaceholderSection section)
    : brls::Activity(create_placeholder_content(section))
    , section(std::move(section)) {
    registerExitAction();
}

}  // namespace switchbox::app
