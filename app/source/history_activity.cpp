#include "switchbox/app/history_activity.hpp"

#include <functional>
#include <string>

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/player_activity.hpp"
#include "switchbox/core/app_config.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

brls::DetailCell* create_action_cell(
    const std::string& title,
    const std::string& detail,
    std::function<bool(brls::View*)> click_action) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->registerClickAction(std::move(click_action));
    return cell;
}

std::string history_entry_title(const switchbox::core::PlaybackHistoryEntry& entry) {
    if (!entry.item_title.empty()) {
        return entry.item_title;
    }
    if (!entry.source_title.empty()) {
        return entry.source_title;
    }
    return tr("history_page/untitled");
}

std::string history_entry_detail(const switchbox::core::PlaybackHistoryEntry& entry) {
    std::string detail = entry.source_kind == switchbox::core::PlaybackSourceKind::Smb ? "SMB" : "IPTV";

    if (!entry.source_title.empty()) {
        detail += " · " + entry.source_title;
    }

    if (entry.source_kind == switchbox::core::PlaybackSourceKind::Smb) {
        if (!entry.smb_relative_path.empty()) {
            detail += " / " + entry.smb_relative_path;
        } else if (!entry.item_subtitle.empty()) {
            detail += " / " + entry.item_subtitle;
        }
        return detail;
    }

    if (!entry.iptv_group_title.empty()) {
        detail += " / " + entry.iptv_group_title;
    } else if (!entry.item_subtitle.empty()) {
        detail += " / " + entry.item_subtitle;
    }

    return detail;
}

}  // namespace

HistoryActivity::HistoryActivity()
    : brls::Activity() {
}

brls::View* HistoryActivity::createContentView() {
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(24, 40, 24, 32);
    this->content_box = content;

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(content);

    auto* frame = new brls::AppletFrame(scrolling_frame);
    frame->setTitle(tr("sections/history/title"));
    return frame;
}

void HistoryActivity::onContentAvailable() {
    registerExitAction();
    reload_entries();
}

void HistoryActivity::reload_entries() {
    if (this->content_box == nullptr) {
        return;
    }

    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto& config = switchbox::core::AppConfigStore::current();
    (void)switchbox::core::remove_playback_history_missing_sources(paths, config);
    this->entries = switchbox::core::load_playback_history(paths);

    this->content_box->clearViews();

    auto* header = new brls::Header();
    header->setTitle(tr("sections/history/title"));
    header->setSubtitle(tr("history_page/summary"));
    this->content_box->addView(header);

    if (this->entries.empty()) {
        this->content_box->addView(create_action_cell(
            tr("history_page/empty_title"),
            tr("history_page/empty_detail"),
            [](brls::View*) { return true; }));
        return;
    }

    for (size_t index = 0; index < this->entries.size(); ++index) {
        const auto entry = this->entries[index];
        this->content_box->addView(create_action_cell(
            history_entry_title(entry),
            history_entry_detail(entry),
            [this, index](brls::View*) {
                open_history_entry(index);
                return true;
            }));
    }
}

void HistoryActivity::open_history_entry(size_t index) {
    if (index >= this->entries.size()) {
        return;
    }

    switchbox::core::PlaybackTarget target;
    std::string error_message;
    if (!switchbox::core::build_playback_target_from_history_entry(
            switchbox::core::AppConfigStore::current(),
            this->entries[index],
            target,
            error_message)) {
        auto* dialog = new brls::Dialog(
            error_message.empty() ? tr("history_page/open_failed") : error_message);
        dialog->addButton(brls::getStr("hints/ok"), [this]() {
            reload_entries();
        });
        dialog->open();
        return;
    }

    brls::Application::pushActivity(new PlayerActivity(std::move(target)));
}

}  // namespace switchbox::app
