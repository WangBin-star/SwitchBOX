#include "switchbox/app/history_activity.hpp"

#include <functional>
#include <string>

#include <borealis/core/application.hpp>
#include <borealis/core/thread.hpp>
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

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

void focus_dialog_button(brls::Dialog* dialog, const std::string& view_id) {
    if (dialog == nullptr) {
        return;
    }

    if (auto* target = dialog->getView(view_id)) {
        dialog->setLastFocusedView(target);
    }

    brls::delay(1, [dialog, view_id]() {
        if (auto* target = dialog->getView(view_id)) {
            dialog->setLastFocusedView(target);
            brls::Application::giveFocus(target);
        }
    });
}

std::string truncate_utf8_with_ellipsis(const std::string& text, size_t max_codepoints) {
    if (max_codepoints == 0 || text.empty()) {
        return "";
    }

    size_t codepoints = 0;
    size_t index = 0;
    while (index < text.size() && codepoints < max_codepoints) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        size_t advance = 1;
        if ((lead & 0x80u) == 0x00u) {
            advance = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            advance = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            advance = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            advance = 4;
        }

        if (index + advance > text.size()) {
            advance = 1;
        }

        index += advance;
        ++codepoints;
    }

    if (index >= text.size()) {
        return text;
    }

    return text.substr(0, index) + "...";
}

class HistoryEntryCell : public brls::DetailCell {
public:
    HistoryEntryCell() {
        this->title->setSingleLine(true);
        this->title->setShrink(1.0f);
        this->detail->setSingleLine(true);
        this->detail->setWidth(320.0f);
        this->detail->setMinWidth(320.0f);
        this->detail->setMaxWidth(320.0f);
        this->detail->setShrink(0.0f);
        this->delete_action_id = this->registerAction(
            tr("actions/delete"),
            brls::BUTTON_X,
            [this](brls::View*) {
                return this->delete_listener ? this->delete_listener() : false;
            },
            false,
            false,
            brls::SOUND_CLICK);
        this->setActionAvailable(brls::BUTTON_X, false);
    }

    void setDeleteListener(std::function<bool()> listener) {
        this->delete_listener = std::move(listener);
        this->setActionAvailable(brls::BUTTON_X, this->delete_listener != nullptr);
    }

private:
    brls::ActionIdentifier delete_action_id = ACTION_NONE;
    std::function<bool()> delete_listener;
};

HistoryEntryCell* create_action_cell(
    const std::string& title,
    const std::string& detail,
    std::function<bool(brls::View*)> click_action,
    std::function<bool()> delete_action = nullptr) {
    auto* cell = new HistoryEntryCell();
    cell->setText(title);
    cell->setDetailText(truncate_utf8_with_ellipsis(detail, 42));
    cell->setDeleteListener(std::move(delete_action));
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
    std::string detail = "IPTV";
    if (entry.source_kind == switchbox::core::PlaybackSourceKind::Smb) {
        detail = "SMB";
    } else if (entry.source_kind == switchbox::core::PlaybackSourceKind::WebDav) {
        detail = "WebDAV";
    }

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

    if (entry.source_kind == switchbox::core::PlaybackSourceKind::WebDav) {
        if (!entry.webdav_relative_path.empty()) {
            detail += " / " + entry.webdav_relative_path;
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
    install_common_actions();
    reload_entries();
}

void HistoryActivity::install_common_actions() {
    if (this->action_clear_id != ACTION_NONE) {
        unregisterAction(this->action_clear_id);
        this->action_clear_id = ACTION_NONE;
    }

    this->action_clear_id = registerAction(
        tr("actions/clear"),
        brls::BUTTON_START,
        [this](brls::View*) {
            confirm_clear_history();
            return true;
        },
        false,
        false,
        brls::SOUND_CLICK);
}

void HistoryActivity::update_common_action_availability() {
    if (auto* root = getContentView()) {
        root->setActionAvailable(brls::BUTTON_START, !this->entries.empty());
    }
}

void HistoryActivity::reload_entries(const std::string& preferred_focus_stable_key) {
    if (this->content_box == nullptr) {
        return;
    }

    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto& config = switchbox::core::AppConfigStore::current();
    (void)switchbox::core::remove_playback_history_missing_sources(paths, config);
    this->entries = switchbox::core::load_playback_history(paths);
    update_common_action_availability();

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
        focus_history_entry_by_stable_key({});
        return;
    }

    brls::View* focus_view = nullptr;
    brls::View* fallback_focus_view = nullptr;
    for (size_t index = 0; index < this->entries.size(); ++index) {
        const auto entry = this->entries[index];
        auto* cell = create_action_cell(
            history_entry_title(entry),
            history_entry_detail(entry),
            [this, index](brls::View*) {
                open_history_entry(index);
                return true;
            },
            [this, index]() {
                confirm_delete_history_entry(index);
                return true;
            });
        this->content_box->addView(cell);
        if (!preferred_focus_stable_key.empty() && entry.stable_key == preferred_focus_stable_key) {
            focus_view = cell;
        } else if (fallback_focus_view == nullptr) {
            fallback_focus_view = cell;
        }
    }

    if (focus_view == nullptr) {
        focus_view = fallback_focus_view;
    }

    if (focus_view != nullptr) {
        brls::delay(1, [focus_view]() {
            if (focus_view->getVisibility() == brls::Visibility::VISIBLE) {
                brls::Application::giveFocus(focus_view);
            }
        });
    }
}

void HistoryActivity::focus_history_entry_by_stable_key(const std::string& preferred_focus_stable_key) {
    if (preferred_focus_stable_key.empty()) {
        if (auto* fallback_focus = getDefaultFocus()) {
            brls::delay(1, [fallback_focus]() {
                if (fallback_focus->getVisibility() == brls::Visibility::VISIBLE) {
                    brls::Application::giveFocus(fallback_focus);
                }
            });
        }
    }
}

void HistoryActivity::confirm_delete_history_entry(size_t index) {
    if (index >= this->entries.size()) {
        return;
    }

    const std::string deleting_title = history_entry_title(this->entries[index]);
    auto* dialog = new brls::Dialog(tr("history_page/delete_confirm", deleting_title));
    dialog->addButton(brls::getStr("hints/cancel"), []() {});
    dialog->addButton(
        tr("actions/confirm"),
        [this, index]() {
            const std::string next_focus_stable_key =
                index + 1 < this->entries.size()
                    ? this->entries[index + 1].stable_key
                    : (index > 0 ? this->entries[index - 1].stable_key : std::string{});
            if (!switchbox::core::remove_playback_history_entry(
                    switchbox::core::AppConfigStore::paths(),
                    this->entries[index].stable_key)) {
                brls::Application::notify(tr("history_page/delete_failed"));
                return;
            }
            reload_entries(next_focus_stable_key);
        });
    dialog->open();
    focus_dialog_button(dialog, "brls/dialog/button1");
}

void HistoryActivity::confirm_clear_history() {
    if (this->entries.empty()) {
        return;
    }

    auto* dialog = new brls::Dialog(tr("history_page/clear_confirm"));
    dialog->addButton(brls::getStr("hints/cancel"), []() {});
    dialog->addButton(
        tr("actions/confirm"),
        [this]() {
            if (!switchbox::core::clear_playback_history(switchbox::core::AppConfigStore::paths())) {
                brls::Application::notify(tr("history_page/clear_failed"));
                return;
            }
            reload_entries();
        });
    dialog->open();
    focus_dialog_button(dialog, "brls/dialog/button1");
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
