#include "switchbox/app/config_missing_activity.hpp"

#include <utility>

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"

namespace switchbox::app {

namespace {

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

brls::DetailCell* create_info_cell(const std::string& title, const std::string& detail) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    return cell;
}

void apply_native_status_layout(brls::AppletFrame* frame) {
    frame->setTitle("SwitchBOX");

#ifndef __SWITCH__
    if (auto* timeView = frame->getView("brls/hints/time")) {
        timeView->setVisibility(brls::Visibility::GONE);
    }

    if (auto* wirelessView = frame->getView("brls/wireless")) {
        wirelessView->setVisibility(brls::Visibility::GONE);
    }

    if (auto* batteryView = frame->getView("brls/battery")) {
        batteryView->setVisibility(brls::Visibility::GONE);
    }
#endif
}

brls::View* create_missing_config_content(const std::vector<std::filesystem::path>& searchedPaths) {
    auto theme = brls::Application::getTheme();

    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(30, 40, 30, 40);

    auto* eyebrow = create_label("启动检查", 15.0f, theme["brls/highlight/color2"], true);
    eyebrow->setMargins(0, 0, 8, 0);
    container->addView(eyebrow);

    auto* header = new brls::Header();
    header->setTitle("安装不完整");
    header->setSubtitle("缺少必要的 ini 文件");
    container->addView(header);

    auto* summary = create_label(
        "缺少必要的 ini 文件，安装不完整，请将 zip 包的所有内容一起复制到 switch 路径下。",
        19.0f,
        theme["brls/text"]);
    summary->setMargins(0, 0, 14, 0);
    container->addView(summary);

    container->addView(create_info_cell("建议检查", "请确认 .nro、switchbox.ini、langs/ 已一起复制"));

    auto* searchTitle = create_label("已尝试查找的 ini 路径", 24.0f, theme["brls/text"], true);
    searchTitle->setMargins(14, 0, 4, 0);
    container->addView(searchTitle);

    for (size_t index = 0; index < searchedPaths.size(); index++) {
        container->addView(create_info_cell(
            "路径 " + std::to_string(index + 1),
            searchedPaths[index].string()));
    }

    auto* scrollingFrame = new brls::ScrollingFrame();
    scrollingFrame->setContentView(container);
#ifndef __SWITCH__
    scrollingFrame->getAppletFrameItem()->setHintView(new HeaderStatusHint());
#endif

    auto* frame = new brls::AppletFrame(scrollingFrame);
    apply_native_status_layout(frame);
    return frame;
}

}  // namespace

ConfigMissingActivity::ConfigMissingActivity(std::vector<std::filesystem::path> searched_paths)
    : brls::Activity(create_missing_config_content(searched_paths))
    , searched_paths(std::move(searched_paths)) {
    registerExitAction();
}

}  // namespace switchbox::app
