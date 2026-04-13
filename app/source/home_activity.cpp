#include "switchbox/app/home_activity.hpp"

#include <memory>
#include <string>

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>

#include "switchbox/core/build_info.hpp"

namespace switchbox::app {

namespace {

brls::View* create_home_content(const StartupContext& context) {
    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(30, 40, 30, 40);

    auto* header = new brls::Header();
    header->setTitle("SwitchBOX");
    header->setSubtitle("Switch-first media player shell");
    container->addView(header);

    auto* summary = new brls::Label();
    summary->setText(
        "Current bootstrap: Borealis is wired, desktop debug shell is live, and Switch builds can generate .nro.");
    container->addView(summary);

    auto* platformCell = new brls::DetailCell();
    platformCell->setText("Platform");
    platformCell->setDetailText(context.platform_name);
    container->addView(platformCell);

    auto* versionCell = new brls::DetailCell();
    versionCell->setText("Version");
    versionCell->setDetailText(switchbox::core::BuildInfo::version_string());
    container->addView(versionCell);

    auto* nextCell = new brls::DetailCell();
    nextCell->setText("Next milestone");
    nextCell->setDetailText("Playback shell, then IPTV, then SMB validation");
    container->addView(nextCell);

    auto* runtimeCell = new brls::DetailCell();
    runtimeCell->setText("Runtime mode");
    runtimeCell->setDetailText(context.switch_target ? "Switch app target" : "Desktop debug target");
    container->addView(runtimeCell);

    auto* frame = new brls::AppletFrame(container);
    frame->setTitle("SwitchBOX");
    return frame;
}

}  // namespace

HomeActivity::HomeActivity(const StartupContext& context)
    : brls::Activity(create_home_content(context)) {
    registerExitAction();
}

}  // namespace switchbox::app
