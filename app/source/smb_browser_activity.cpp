#include "switchbox/app/smb_browser_activity.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <borealis/core/box.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/app/player_activity.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/playback_target.hpp"
#include "switchbox/core/smb_browser.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::string visible_smb_title(const switchbox::core::SmbSourceSettings& source) {
    if (!source.title.empty()) {
        return source.title;
    }

    return tr("home/cards/common/untitled_smb");
}

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

brls::DetailCell* create_detail_cell(
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

#ifndef __SWITCH__
    if (auto* time_view = frame->getView("brls/hints/time")) {
        time_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* wireless_view = frame->getView("brls/wireless")) {
        wireless_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* battery_view = frame->getView("brls/battery")) {
        battery_view->setVisibility(brls::Visibility::GONE);
    }
#endif
}

void apply_header_path(
    brls::AppletFrame* frame,
    const std::string& path,
    NVGcolor color) {
    if (path.empty()) {
        return;
    }

    auto& header_children = frame->getHeader()->getChildren();
    if (header_children.empty()) {
        return;
    }

    auto* left_header_box = dynamic_cast<brls::Box*>(header_children.front());
    if (left_header_box == nullptr) {
        return;
    }

    auto style = brls::Application::getStyle();
    const float title_font_size = style["brls/applet_frame/header_title_font_size"];

    left_header_box->setAlignItems(brls::AlignItems::FLEX_END);

    auto* path_label = create_label(path, 13.0f, color, true);
    path_label->setMargins(0, 0, 0, title_font_size);
    left_header_box->addView(path_label);
}

void apply_footer_extensions_hint(
    brls::AppletFrame* frame,
    const std::string& text,
    NVGcolor color) {
    if (text.empty()) {
        return;
    }

    auto* footer = frame->getFooter();
    if (footer == nullptr) {
        return;
    }

    auto* bottom_bar = dynamic_cast<brls::BottomBar*>(footer);
    if (bottom_bar == nullptr) {
        return;
    }

    auto* extensions_label = create_label(text, 14.0f, color, false);
    extensions_label->setMaxWidth(560.0f);
    extensions_label->setLineHeight(18.0f);
    bottom_bar->setLeftView(extensions_label);
}

std::string format_bytes(std::uintmax_t size) {
    static constexpr const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};

    double value = static_cast<double>(size);
    size_t suffix_index = 0;

    while (value >= 1024.0 && suffix_index + 1 < std::size(suffixes)) {
        value /= 1024.0;
        ++suffix_index;
    }

    std::ostringstream stream;
    if (suffix_index == 0) {
        stream << static_cast<std::uintmax_t>(value) << ' ' << suffixes[suffix_index];
    } else {
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream << value << ' ' << suffixes[suffix_index];
    }

    return stream.str();
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

std::string format_entry_title(const switchbox::core::SmbBrowserEntry& entry) {
    return entry.name;
}

void attach_directory_icon_prefix(brls::DetailCell* cell, NVGcolor icon_color) {
    if (cell == nullptr || cell->title == nullptr) {
        return;
    }

    // Material Icons "folder" (U+E2C7), keep it as a standalone label so we can
    // control vertical offset and align it with entry text center.
    auto* icon_label = create_label("\uE2C7", cell->title->getFontSize(), icon_color, true);
    icon_label->setLineHeight(cell->title->getLineHeight());
    const float title_height = cell->title->getFontSize() * cell->title->getLineHeight();
    const float icon_down_offset = std::max(1.0f, title_height / 6.0f);
    icon_label->setMargins(icon_down_offset, 8.0f, 0.0f, 0.0f);
    cell->addView(icon_label, 0);
}

struct SmbBrowserContentBuild {
    brls::View* view = nullptr;
    brls::View* focus_view = nullptr;
    std::vector<switchbox::core::SmbBrowserEntry> rendered_entries;
    bool content_from_server = false;
};

enum class PendingRefreshKind {
    None,
    LocalDeleteOnly,
    RefreshFromServer,
};

struct PendingDeleteRefreshRequest {
    bool pending = false;
    PendingRefreshKind kind = PendingRefreshKind::None;
    switchbox::core::SmbSourceSettings source;
    std::string directory_relative_path;
    std::string focus_relative_path;
    std::string deleted_relative_path;
};

PendingDeleteRefreshRequest g_pending_delete_refresh;

std::string make_entry_view_id(const std::string& relative_path) {
    return "switchbox/smb_entry/" + switchbox::core::smb_join_relative_path({}, relative_path);
}

bool same_smb_source(
    const switchbox::core::SmbSourceSettings& lhs,
    const switchbox::core::SmbSourceSettings& rhs) {
    auto normalize = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    };

    // Match by actual SMB connection identity, not UI-only fields like key/title.
    return normalize(lhs.host) == normalize(rhs.host) &&
           normalize(lhs.share) == normalize(rhs.share);
}

brls::ScrollingFrame* find_scrolling_frame_from_root(brls::View* root) {
    auto* frame = dynamic_cast<brls::AppletFrame*>(root);
    if (frame == nullptr) {
        return nullptr;
    }

    return dynamic_cast<brls::ScrollingFrame*>(frame->getContentView());
}

SmbBrowserContentBuild create_smb_browser_content(
    const switchbox::core::SmbSourceSettings& source,
    const std::string& relative_path,
    const std::vector<switchbox::core::SmbBrowserEntry>& entries,
    const std::string& preferred_focus_relative_path = {}) {
    auto theme = brls::Application::getTheme();
    const auto& config = switchbox::core::AppConfigStore::current();

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(18, 40, 30, 40);

    brls::View* focus_view = nullptr;
    brls::View* fallback_focus_view = nullptr;
    const bool should_restore_focus = !preferred_focus_relative_path.empty();
    if (entries.empty()) {
        auto* empty_label = create_label(
            tr("smb_browser/empty"),
            17.0f,
            theme["brls/text_disabled"]);
        empty_label->setMargins(8, 10, 14, 14);
        content->addView(empty_label);
    }

    for (const auto& entry : entries) {
        const std::string title = format_entry_title(entry);
        const std::string detail = entry.is_directory ? tr("smb_browser/folder_detail")
                                                      : tr(
                                                            "smb_browser/file_detail",
                                                            format_bytes(entry.size));

        auto* cell = create_detail_cell(
            title,
            detail,
            entry.is_directory ? theme["brls/highlight/color2"]
                               : theme["brls/list/listItem_value_color"]);
        if (entry.is_directory) {
            attach_directory_icon_prefix(cell, theme["brls/highlight/color2"]);
        }
        cell->setId(make_entry_view_id(entry.relative_path));
        cell->registerClickAction([source, entry](brls::View*) {
            if (entry.is_directory) {
                brls::Application::pushActivity(new SmbBrowserActivity(source, entry.relative_path));
                return true;
            }

            brls::Application::pushActivity(new PlayerActivity(
                switchbox::core::make_smb_playback_target(source, entry.relative_path)));
            return true;
        });
        content->addView(cell);

        if (!preferred_focus_relative_path.empty() &&
            entry.relative_path == preferred_focus_relative_path) {
            focus_view = cell;
        } else if (should_restore_focus && fallback_focus_view == nullptr && !entry.is_directory) {
            fallback_focus_view = cell;
        } else if (should_restore_focus && fallback_focus_view == nullptr) {
            fallback_focus_view = cell;
        }
    }

    if (should_restore_focus && focus_view == nullptr) {
        focus_view = fallback_focus_view;
    }

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(content);
#ifndef __SWITCH__
    scrolling_frame->getAppletFrameItem()->setHintView(new HeaderStatusHint());
#endif

    auto* frame = new brls::AppletFrame(scrolling_frame);
    apply_native_status_layout(frame, visible_smb_title(source));
    apply_header_path(
        frame,
        switchbox::core::smb_display_path(source, relative_path),
        theme["brls/text_disabled"]);
    apply_footer_extensions_hint(
        frame,
        tr("smb_browser/extensions_hint", config.general.playable_extensions),
        theme["brls/text_disabled"]);
    return {
        .view = frame,
        .focus_view = focus_view,
        .rendered_entries = entries,
        .content_from_server = false,
    };
}

SmbBrowserContentBuild create_smb_browser_content(
    const switchbox::core::SmbSourceSettings& source,
    const std::string& relative_path,
    const std::string& preferred_focus_relative_path = {},
    const std::string& hidden_relative_path = {}) {
    auto theme = brls::Application::getTheme();
    const auto& config = switchbox::core::AppConfigStore::current();
    const auto browse_result =
        switchbox::core::browse_smb_directory(source, config.general, relative_path);

    if (!browse_result.backend_available) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(18, 40, 30, 40);
        auto* message = create_label(
            tr("smb_browser/backend_unavailable"),
            18.0f,
            theme["brls/text"]);
        message->setMargins(0, 10, 10, 0);
        content->addView(message);
        auto* scrolling_frame = new brls::ScrollingFrame();
        scrolling_frame->setContentView(content);
#ifndef __SWITCH__
        scrolling_frame->getAppletFrameItem()->setHintView(new HeaderStatusHint());
#endif

        auto* frame = new brls::AppletFrame(scrolling_frame);
        apply_native_status_layout(frame, visible_smb_title(source));
        apply_header_path(
            frame,
            switchbox::core::smb_display_path(source, relative_path),
            theme["brls/text_disabled"]);
        apply_footer_extensions_hint(
            frame,
            tr("smb_browser/extensions_hint", config.general.playable_extensions),
            theme["brls/text_disabled"]);
        return {
            .view = frame,
            .focus_view = nullptr,
            .rendered_entries = {},
            .content_from_server = false,
        };
    }

    if (!browse_result.success) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(18, 40, 30, 40);
        auto* message = create_label(
            tr("smb_browser/open_failed"),
            18.0f,
            theme["brls/text"]);
        message->setMargins(0, 10, 6, 0);
        content->addView(message);

        auto* detail = create_label(
            browse_result.error_message,
            15.0f,
            theme["brls/text_disabled"]);
        detail->setMargins(0, 0, 14, 0);
        content->addView(detail);

#if defined(_WIN32) && !defined(__SWITCH__)
        auto* desktop_note = create_label(
            tr("smb_browser/desktop_note"),
            15.0f,
            theme["brls/text_disabled"]);
        desktop_note->setMargins(0, 0, 10, 0);
        content->addView(desktop_note);
#endif

        auto* scrolling_frame = new brls::ScrollingFrame();
        scrolling_frame->setContentView(content);
#ifndef __SWITCH__
        scrolling_frame->getAppletFrameItem()->setHintView(new HeaderStatusHint());
#endif

        auto* frame = new brls::AppletFrame(scrolling_frame);
        apply_native_status_layout(frame, visible_smb_title(source));
        apply_header_path(
            frame,
            switchbox::core::smb_display_path(source, relative_path),
            theme["brls/text_disabled"]);
        apply_footer_extensions_hint(
            frame,
            tr("smb_browser/extensions_hint", config.general.playable_extensions),
            theme["brls/text_disabled"]);
        return {
            .view = frame,
            .focus_view = nullptr,
            .rendered_entries = {},
            .content_from_server = false,
        };
    }

    std::vector<switchbox::core::SmbBrowserEntry> rendered_entries;
    rendered_entries.reserve(browse_result.entries.size());
    for (const auto& entry : browse_result.entries) {
        if (!hidden_relative_path.empty() && entry.relative_path == hidden_relative_path) {
            continue;
        }
        rendered_entries.push_back(entry);
    }

    auto built = create_smb_browser_content(
        source,
        relative_path,
        rendered_entries,
        preferred_focus_relative_path);
    built.content_from_server = true;
    return built;
}

}  // namespace

void SmbBrowserActivity::request_focus_after_return(
    const switchbox::core::SmbSourceSettings& source,
    std::string directory_relative_path,
    std::string focus_relative_path,
    std::string deleted_relative_path) {
    g_pending_delete_refresh.pending = true;
    g_pending_delete_refresh.kind = PendingRefreshKind::LocalDeleteOnly;
    g_pending_delete_refresh.source = source;
    g_pending_delete_refresh.directory_relative_path =
        switchbox::core::smb_join_relative_path({}, directory_relative_path);
    g_pending_delete_refresh.focus_relative_path =
        switchbox::core::smb_join_relative_path({}, focus_relative_path);
    g_pending_delete_refresh.deleted_relative_path =
        switchbox::core::smb_join_relative_path({}, deleted_relative_path);
}

void SmbBrowserActivity::request_refresh_after_return(
    const switchbox::core::SmbSourceSettings& source,
    std::string directory_relative_path) {
    g_pending_delete_refresh.pending = true;
    g_pending_delete_refresh.kind = PendingRefreshKind::RefreshFromServer;
    g_pending_delete_refresh.source = source;
    g_pending_delete_refresh.directory_relative_path =
        switchbox::core::smb_join_relative_path({}, directory_relative_path);
    g_pending_delete_refresh.focus_relative_path.clear();
    g_pending_delete_refresh.deleted_relative_path.clear();
}

SmbBrowserActivity::SmbBrowserActivity(
    switchbox::core::SmbSourceSettings source,
    std::string relative_path)
    : brls::Activity(create_smb_browser_content(source, relative_path).view)
    , source(std::move(source))
    , relative_path(std::move(relative_path)) {
    const auto& config = switchbox::core::AppConfigStore::current();
    const auto initial_result =
        switchbox::core::browse_smb_directory(this->source, config.general, this->relative_path);
    if (initial_result.success) {
        this->cached_entries = initial_result.entries;
        this->has_cached_entries = true;
    } else {
        this->cached_entries.clear();
        this->has_cached_entries = false;
    }
}

SmbBrowserActivity::~SmbBrowserActivity() = default;

void SmbBrowserActivity::onContentAvailable() {
    brls::Activity::onContentAvailable();
    install_common_actions();
    bind_entry_focus_tracking();
}

void SmbBrowserActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    install_common_actions();
    consume_pending_refresh_if_any();
}

void SmbBrowserActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    if (!this->relative_path.empty()) {
        SmbBrowserActivity::request_refresh_after_return(
            this->source,
            switchbox::core::smb_parent_relative_path(this->relative_path));
    }
}

void SmbBrowserActivity::onResume() {
    brls::Activity::onResume();
    install_common_actions();
    consume_pending_refresh_if_any();
}

void SmbBrowserActivity::sync_focused_entry_from_ui() {
    auto* focused_view = brls::Application::getCurrentFocus();
    if (focused_view == nullptr || focused_view->getParentActivity() != this) {
        return;
    }

    for (const auto& entry : this->cached_entries) {
        auto* entry_view = this->getView(make_entry_view_id(entry.relative_path));
        if (entry_view == nullptr) {
            continue;
        }

        for (auto* cursor = focused_view; cursor != nullptr; cursor = cursor->getParent()) {
            if (cursor == entry_view) {
                this->focused_entry_relative_path =
                    switchbox::core::smb_join_relative_path({}, entry.relative_path);
                this->focused_entry_is_directory = entry.is_directory;
                return;
            }
        }
    }
}

void SmbBrowserActivity::apply_local_delete_result(
    const std::string& deleted_relative_path,
    const std::string& preferred_focus_relative_path) {
    const std::string normalized_deleted =
        switchbox::core::smb_join_relative_path({}, deleted_relative_path);

    auto* deleted_view = getView(make_entry_view_id(normalized_deleted));
    if (deleted_view != nullptr) {
        deleted_view->setVisibility(brls::Visibility::GONE);
    }

    this->cached_entries.erase(
        std::remove_if(
            this->cached_entries.begin(),
            this->cached_entries.end(),
            [&normalized_deleted](const switchbox::core::SmbBrowserEntry& entry) {
                return switchbox::core::smb_join_relative_path({}, entry.relative_path) ==
                       normalized_deleted;
            }),
        this->cached_entries.end());
    this->has_cached_entries = true;

    std::string next_focus =
        switchbox::core::smb_join_relative_path({}, preferred_focus_relative_path);
    if (next_focus.empty() && !this->cached_entries.empty()) {
        next_focus =
            switchbox::core::smb_join_relative_path({}, this->cached_entries.front().relative_path);
    }

    this->focused_entry_relative_path.clear();
    this->focused_entry_is_directory = false;

    if (!next_focus.empty()) {
        if (auto* next_view = getView(make_entry_view_id(next_focus)); next_view != nullptr) {
            brls::Application::giveFocus(next_view);
        }

        for (const auto& entry : this->cached_entries) {
            if (switchbox::core::smb_join_relative_path({}, entry.relative_path) == next_focus) {
                this->focused_entry_relative_path = next_focus;
                this->focused_entry_is_directory = entry.is_directory;
                break;
            }
        }
    }
}

void SmbBrowserActivity::install_common_actions() {
    if (this->action_back_id != ACTION_NONE) {
        unregisterAction(this->action_back_id);
        this->action_back_id = ACTION_NONE;
    }
    if (this->action_delete_id != ACTION_NONE) {
        unregisterAction(this->action_delete_id);
        this->action_delete_id = ACTION_NONE;
    }
    if (this->action_home_id != ACTION_NONE) {
        unregisterAction(this->action_home_id);
        this->action_home_id = ACTION_NONE;
    }

    this->action_back_id = registerAction(
        brls::getStr("hints/back"),
        brls::BUTTON_B,
        [](brls::View*) {
            brls::Application::popActivity(brls::TransitionAnimation::FADE);
            return true;
        });
    this->action_delete_id = registerAction(
        tr("actions/delete"),
        brls::BUTTON_X,
        [this](brls::View*) {
            confirm_delete_focused_entry();
            return true;
        });
    this->action_home_id = registerAction(
        tr("actions/home"),
        brls::BUTTON_Y,
        [this](brls::View*) {
            return_to_home();
            return true;
        });
}

void SmbBrowserActivity::bind_entry_focus_tracking() {
    this->focused_entry_relative_path.clear();
    this->focused_entry_is_directory = false;

    if (!this->has_cached_entries || this->cached_entries.empty()) {
        return;
    }

    for (const auto& entry : this->cached_entries) {
        auto* view = this->getView(make_entry_view_id(entry.relative_path));
        if (view == nullptr) {
            continue;
        }

        view->getFocusEvent()->subscribe([this, relative_path = entry.relative_path, is_directory = entry.is_directory](brls::View*) {
            this->focused_entry_relative_path =
                switchbox::core::smb_join_relative_path({}, relative_path);
            this->focused_entry_is_directory = is_directory;
        });
    }

    this->focused_entry_relative_path =
        switchbox::core::smb_join_relative_path({}, this->cached_entries.front().relative_path);
    this->focused_entry_is_directory = this->cached_entries.front().is_directory;
}

std::string SmbBrowserActivity::find_next_focus_after_delete(const std::string& deleted_relative_path) const {
    if (this->cached_entries.empty()) {
        return {};
    }

    const std::string normalized_deleted =
        switchbox::core::smb_join_relative_path({}, deleted_relative_path);
    int deleted_index = -1;
    for (int index = 0; index < static_cast<int>(this->cached_entries.size()); ++index) {
        if (switchbox::core::smb_join_relative_path(
                {},
                this->cached_entries[static_cast<size_t>(index)].relative_path) == normalized_deleted) {
            deleted_index = index;
            break;
        }
    }

    if (deleted_index < 0) {
        return {};
    }

    if (deleted_index + 1 < static_cast<int>(this->cached_entries.size())) {
        return this->cached_entries[static_cast<size_t>(deleted_index + 1)].relative_path;
    }
    if (deleted_index > 0) {
        return this->cached_entries[static_cast<size_t>(deleted_index - 1)].relative_path;
    }
    return {};
}

void SmbBrowserActivity::confirm_delete_focused_entry() {
    sync_focused_entry_from_ui();

    if (this->focused_entry_relative_path.empty()) {
        brls::Application::notify(tr("smb_browser/empty"));
        return;
    }

    const std::string deleting_relative_path =
        switchbox::core::smb_join_relative_path({}, this->focused_entry_relative_path);
    const std::string deleting_file_name = file_name_from_relative_path(deleting_relative_path);
    const std::string confirm_message =
        deleting_file_name.empty()
            ? tr("player_page/delete/confirm_generic")
            : tr("player_page/delete/confirm_named", deleting_file_name);

    auto* dialog = new brls::Dialog(confirm_message);
    dialog->addButton(brls::getStr("hints/cancel"), []() {});
    dialog->addButton(brls::getStr("hints/ok"), [this, deleting_relative_path]() {
        const std::string next_focus = find_next_focus_after_delete(deleting_relative_path);
        std::string error_message;
        if (!switchbox::core::delete_smb_file(this->source, deleting_relative_path, error_message)) {
            if (error_message.empty()) {
                error_message = "Failed to delete SMB file.";
            }
            auto* failed = new brls::Dialog(error_message);
            failed->open();
            return;
        }

        apply_local_delete_result(deleting_relative_path, next_focus);
    });
    dialog->open();
}

void SmbBrowserActivity::return_to_home() {
    auto pop_all = std::make_shared<std::function<void()>>();
    *pop_all = [pop_all]() {
        if (!brls::Application::popActivity(
                brls::TransitionAnimation::FADE,
                [pop_all]() {
                    (*pop_all)();
                })) {
            return;
        }
    };
    (*pop_all)();
}

bool SmbBrowserActivity::consume_pending_refresh_if_any() {
    if (!g_pending_delete_refresh.pending) {
        return false;
    }

    if (!same_smb_source(g_pending_delete_refresh.source, this->source)) {
        return false;
    }

    const std::string directory = g_pending_delete_refresh.directory_relative_path;
    const std::string focus = g_pending_delete_refresh.focus_relative_path;
    const std::string hidden = g_pending_delete_refresh.deleted_relative_path;
    const PendingRefreshKind kind = g_pending_delete_refresh.kind;
    const std::string normalized_directory =
        switchbox::core::smb_join_relative_path({}, directory);
    const std::string normalized_current =
        switchbox::core::smb_join_relative_path({}, this->relative_path);

    if (kind == PendingRefreshKind::RefreshFromServer) {
        if (normalized_directory != normalized_current) {
            return false;
        }
        g_pending_delete_refresh.pending = false;
        g_pending_delete_refresh.kind = PendingRefreshKind::None;
        g_pending_delete_refresh.deleted_relative_path.clear();
        refresh_after_player_delete(normalized_directory, {});
        return true;
    }

    // Rule: when returning from player to this same list, do local immediate update
    // (hide deleted item + focus restore) without server sync.
    if (kind == PendingRefreshKind::LocalDeleteOnly) {
        if (normalized_directory != normalized_current) {
            return false;
        }
        g_pending_delete_refresh.pending = false;
        g_pending_delete_refresh.kind = PendingRefreshKind::None;
        g_pending_delete_refresh.deleted_relative_path.clear();

        if (hidden.empty() || !this->has_cached_entries) {
            return true;
        }

        float previous_scroll_offset = 0.0f;
        if (auto* scrolling = find_scrolling_frame_from_root(this->getContentView())) {
            previous_scroll_offset = scrolling->getContentOffsetY();
        }

        std::vector<switchbox::core::SmbBrowserEntry> updated_entries = this->cached_entries;
        std::string fallback_focus_relative_path = focus;
        const std::string normalized_hidden =
            switchbox::core::smb_join_relative_path({}, hidden);
        int deleted_index = -1;
        for (int index = 0; index < static_cast<int>(updated_entries.size()); ++index) {
            if (switchbox::core::smb_join_relative_path(
                    {},
                    updated_entries[static_cast<size_t>(index)].relative_path) == normalized_hidden) {
                deleted_index = index;
                break;
            }
        }

        if (deleted_index >= 0) {
            updated_entries.erase(updated_entries.begin() + deleted_index);
            if (fallback_focus_relative_path.empty()) {
                if (deleted_index < static_cast<int>(updated_entries.size())) {
                    fallback_focus_relative_path =
                        updated_entries[static_cast<size_t>(deleted_index)].relative_path;
                } else if (!updated_entries.empty()) {
                    fallback_focus_relative_path = updated_entries.back().relative_path;
                }
            }
        }

        auto rebuilt = create_smb_browser_content(
            this->source,
            this->relative_path,
            updated_entries,
            fallback_focus_relative_path);
        setContentView(rebuilt.view);
        install_common_actions();
        this->cached_entries = std::move(updated_entries);
        this->has_cached_entries = true;
        bind_entry_focus_tracking();

        if (auto* scrolling = find_scrolling_frame_from_root(this->getContentView())) {
            scrolling->setContentOffsetY(previous_scroll_offset, false);
        }
        if (rebuilt.focus_view != nullptr) {
            brls::Application::giveFocus(rebuilt.focus_view);
        }
        return true;
    }

    return false;
}

void SmbBrowserActivity::refresh_after_player_delete(
    const std::string& directory_relative_path,
    const std::string& focus_relative_path,
    const std::string& deleted_relative_path) {
    const std::string normalized_directory =
        switchbox::core::smb_join_relative_path({}, directory_relative_path);
    const std::string normalized_current =
        switchbox::core::smb_join_relative_path({}, this->relative_path);

    const bool keep_scroll = normalized_directory == normalized_current;
    float previous_scroll_offset = 0.0f;
    if (keep_scroll) {
        if (auto* scrolling = find_scrolling_frame_from_root(this->getContentView())) {
            previous_scroll_offset = scrolling->getContentOffsetY();
        }
    }

    this->relative_path = normalized_directory;
    auto rebuilt = create_smb_browser_content(
        this->source,
        this->relative_path,
        focus_relative_path,
        deleted_relative_path);
    setContentView(rebuilt.view);
    install_common_actions();
    this->cached_entries = std::move(rebuilt.rendered_entries);
    this->has_cached_entries = rebuilt.content_from_server;
    bind_entry_focus_tracking();

    if (auto* scrolling = find_scrolling_frame_from_root(this->getContentView())) {
        scrolling->setContentOffsetY(keep_scroll ? previous_scroll_offset : 0.0f, false);
    }

    if (rebuilt.focus_view != nullptr) {
        brls::Application::giveFocus(rebuilt.focus_view);
    }
}

}  // namespace switchbox::app
