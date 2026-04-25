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
#include <borealis/core/thread.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

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

brls::Label* create_focusable_message_label(
    const std::string& text,
    float font_size,
    NVGcolor color) {
    auto* label = create_label(text, font_size, color, false);
    label->setFocusable(true);
    label->setHideHighlightBackground(true);
    label->setHideHighlightBorder(true);
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
    FocusAfterPlayerExit,
    DeleteAfterPlayerExit,
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
    int focus_view_index = -1;
    int fallback_focus_view_index = -1;
    const bool should_restore_focus = !preferred_focus_relative_path.empty();
    if (entries.empty()) {
        auto* empty_label = create_focusable_message_label(
            tr("smb_browser/empty"),
            17.0f,
            theme["brls/text_disabled"]);
        empty_label->setMargins(8, 10, 14, 14);
        content->addView(empty_label);
        focus_view = empty_label;
        focus_view_index = 0;
    }

    for (size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
        const auto& entry = entries[entry_index];
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
            focus_view_index = static_cast<int>(entry_index);
        } else if (should_restore_focus && fallback_focus_view == nullptr && !entry.is_directory) {
            fallback_focus_view = cell;
            fallback_focus_view_index = static_cast<int>(entry_index);
        } else if (should_restore_focus && fallback_focus_view == nullptr) {
            fallback_focus_view = cell;
            fallback_focus_view_index = static_cast<int>(entry_index);
        }
    }

    if (should_restore_focus && focus_view == nullptr) {
        focus_view = fallback_focus_view;
        focus_view_index = fallback_focus_view_index;
    }

    if (focus_view_index >= 0) {
        content->setDefaultFocusedIndex(focus_view_index);
    }

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(content);

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
        auto* message = create_focusable_message_label(
            tr("smb_browser/backend_unavailable"),
            18.0f,
            theme["brls/text"]);
        message->setMargins(0, 10, 10, 0);
        content->addView(message);
        auto* scrolling_frame = new brls::ScrollingFrame();
        scrolling_frame->setContentView(content);

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
            .focus_view = message,
            .rendered_entries = {},
            .content_from_server = false,
        };
    }

    if (!browse_result.success) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(18, 40, 30, 40);
        auto* message = create_focusable_message_label(
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

        auto* scrolling_frame = new brls::ScrollingFrame();
        scrolling_frame->setContentView(content);

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
            .focus_view = message,
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
    g_pending_delete_refresh.kind = PendingRefreshKind::FocusAfterPlayerExit;
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
    std::string directory_relative_path,
    std::string focus_relative_path) {
    g_pending_delete_refresh.pending = true;
    g_pending_delete_refresh.kind = PendingRefreshKind::RefreshFromServer;
    g_pending_delete_refresh.source = source;
    g_pending_delete_refresh.directory_relative_path =
        switchbox::core::smb_join_relative_path({}, directory_relative_path);
    g_pending_delete_refresh.focus_relative_path =
        switchbox::core::smb_join_relative_path({}, focus_relative_path);
    g_pending_delete_refresh.deleted_relative_path.clear();
}

void SmbBrowserActivity::request_delete_after_return(
    const switchbox::core::SmbSourceSettings& source,
    std::string directory_relative_path,
    std::string focus_relative_path,
    std::string deleted_relative_path) {
    g_pending_delete_refresh.pending = true;
    g_pending_delete_refresh.kind = PendingRefreshKind::DeleteAfterPlayerExit;
    g_pending_delete_refresh.source = source;
    g_pending_delete_refresh.directory_relative_path =
        switchbox::core::smb_join_relative_path({}, directory_relative_path);
    g_pending_delete_refresh.focus_relative_path =
        switchbox::core::smb_join_relative_path({}, focus_relative_path);
    g_pending_delete_refresh.deleted_relative_path =
        switchbox::core::smb_join_relative_path({}, deleted_relative_path);
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
            switchbox::core::smb_parent_relative_path(this->relative_path),
            this->relative_path);
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

void SmbBrowserActivity::update_focused_entry_state(const std::string& relative_path) {
    const std::string normalized_relative_path =
        switchbox::core::smb_join_relative_path({}, relative_path);
    this->focused_entry_relative_path.clear();
    this->focused_entry_is_directory = false;

    if (normalized_relative_path.empty()) {
        return;
    }

    for (const auto& entry : this->cached_entries) {
        if (switchbox::core::smb_join_relative_path({}, entry.relative_path) ==
            normalized_relative_path) {
            this->focused_entry_relative_path = normalized_relative_path;
            this->focused_entry_is_directory = entry.is_directory;
            return;
        }
    }
}

void SmbBrowserActivity::ensure_view_visible(brls::View* focus_target) {
    if (focus_target == nullptr) {
        return;
    }

    auto* scrolling = find_scrolling_frame_from_root(this->getContentView());
    if (scrolling == nullptr) {
        return;
    }

    float local_y = focus_target->getLocalY();
    float item_height = focus_target->getHeight();
    brls::View* parent = focus_target->getParent();
    while (parent != nullptr && dynamic_cast<brls::ScrollingFrame*>(parent->getParent()) == nullptr) {
        local_y += parent->getLocalY();
        parent = parent->getParent();
    }

    const float frame_height = scrolling->getHeight();
    if (frame_height <= 0.0f) {
        return;
    }

    const float visible_top = scrolling->getContentOffsetY();
    const float visible_bottom = visible_top + frame_height;
    float next_offset = visible_top;

    if (local_y < visible_top) {
        next_offset = local_y;
    } else if (local_y + item_height > visible_bottom) {
        next_offset = local_y + item_height - frame_height;
    } else {
        return;
    }

    scrolling->setContentOffsetY(std::max(0.0f, next_offset), false);
}

bool SmbBrowserActivity::restore_focus_now(const std::string& preferred_focus_relative_path) {
    const std::string normalized_focus =
        switchbox::core::smb_join_relative_path({}, preferred_focus_relative_path);
    brls::View* focus_target = nullptr;
    std::string restored_focus_relative_path;

    if (!normalized_focus.empty()) {
        focus_target = this->getView(make_entry_view_id(normalized_focus));
        if (focus_target != nullptr && focus_target->getVisibility() == brls::Visibility::VISIBLE) {
            restored_focus_relative_path = normalized_focus;
        } else {
            focus_target = nullptr;
        }
    }

    if (focus_target == nullptr && !this->cached_entries.empty()) {
        std::string fallback_relative_path;
        for (const auto& entry : this->cached_entries) {
            const std::string candidate =
                switchbox::core::smb_join_relative_path({}, entry.relative_path);
            if (fallback_relative_path.empty()) {
                fallback_relative_path = candidate;
            }
            if (!entry.is_directory) {
                fallback_relative_path = candidate;
                break;
            }
        }

        if (!fallback_relative_path.empty()) {
            focus_target = this->getView(make_entry_view_id(fallback_relative_path));
            if (focus_target != nullptr &&
                focus_target->getVisibility() == brls::Visibility::VISIBLE) {
                restored_focus_relative_path = fallback_relative_path;
            } else {
                focus_target = nullptr;
            }
        }
    }

    if (focus_target == nullptr) {
        focus_target = this->getDefaultFocus();
        if (focus_target != nullptr && focus_target->getVisibility() != brls::Visibility::VISIBLE) {
            focus_target = nullptr;
        }
    }

    if (focus_target == nullptr) {
        return false;
    }

    this->ensure_view_visible(focus_target);
    brls::Application::giveFocus(focus_target);
    if (!restored_focus_relative_path.empty()) {
        this->update_focused_entry_state(restored_focus_relative_path);
    } else {
        this->sync_focused_entry_from_ui();
    }

    auto* current_focus = brls::Application::getCurrentFocus();
    return current_focus != nullptr && current_focus->getParentActivity() == this;
}

void SmbBrowserActivity::schedule_focus_restore(
    const std::string& preferred_focus_relative_path,
    int attempts_remaining) {
    const std::string normalized_focus =
        switchbox::core::smb_join_relative_path({}, preferred_focus_relative_path);
    auto* self = this;
    brls::delay(1, [self, normalized_focus, attempts_remaining]() {
        const auto activities = brls::Application::getActivitiesStack();
        if (activities.empty() || activities.back() != self) {
            return;
        }

        if (!self->restore_focus_now(normalized_focus) && attempts_remaining > 1) {
            self->schedule_focus_restore(normalized_focus, attempts_remaining - 1);
        }
    });
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

    if (!restore_focus_now(next_focus)) {
        schedule_focus_restore(next_focus);
    }
}

bool SmbBrowserActivity::handle_back_action() {
    const std::string parent_relative_path =
        switchbox::core::smb_parent_relative_path(this->relative_path);
    const std::string normalized_parent_relative_path =
        switchbox::core::smb_join_relative_path({}, parent_relative_path);

    SmbBrowserActivity* previous_browser = nullptr;
    const auto activities = brls::Application::getActivitiesStack();
    if (activities.size() >= 2) {
        previous_browser = dynamic_cast<SmbBrowserActivity*>(activities[activities.size() - 2]);
    }

    const bool can_pop_to_previous_browser =
        previous_browser != nullptr &&
        same_smb_source(previous_browser->source, this->source) &&
        switchbox::core::smb_join_relative_path({}, previous_browser->relative_path) ==
            normalized_parent_relative_path;
    if (can_pop_to_previous_browser) {
        brls::Application::popActivity(brls::TransitionAnimation::FADE);
        return true;
    }

    if (this->relative_path.empty()) {
        return_to_home();
        return true;
    }

    const std::string current_relative_path = this->relative_path;
    auto* self = this;
    brls::delay(1, [self, parent_relative_path, current_relative_path]() {
        const auto activities = brls::Application::getActivitiesStack();
        if (activities.empty() || activities.back() != self) {
            return;
        }

        self->refresh_after_player_delete(parent_relative_path, current_relative_path);
    });
    return true;
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
        [this](brls::View*) {
            return handle_back_action();
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

    if (g_pending_delete_refresh.kind != PendingRefreshKind::RefreshFromServer) {
        return false;
    }

    const std::string directory = g_pending_delete_refresh.directory_relative_path;
    const std::string focus = g_pending_delete_refresh.focus_relative_path;
    const std::string normalized_directory =
        switchbox::core::smb_join_relative_path({}, directory);
    const std::string normalized_current =
        switchbox::core::smb_join_relative_path({}, this->relative_path);
    if (normalized_directory != normalized_current) {
        return false;
    }
    g_pending_delete_refresh.pending = false;
    g_pending_delete_refresh.kind = PendingRefreshKind::None;
    g_pending_delete_refresh.deleted_relative_path.clear();
    auto* self = this;
    brls::delay(1, [self, normalized_directory, focus]() {
        const auto activities = brls::Application::getActivitiesStack();
        if (activities.empty() || activities.back() != self) {
            return;
        }
        self->refresh_after_player_delete(normalized_directory, focus);
    });
    return true;
}

bool SmbBrowserActivity::apply_pending_return_from_player_if_any() {
    if (!g_pending_delete_refresh.pending) {
        return false;
    }

    if (!same_smb_source(g_pending_delete_refresh.source, this->source)) {
        return false;
    }

    const PendingRefreshKind kind = g_pending_delete_refresh.kind;
    if (kind != PendingRefreshKind::FocusAfterPlayerExit &&
        kind != PendingRefreshKind::DeleteAfterPlayerExit) {
        return false;
    }

    const std::string directory = g_pending_delete_refresh.directory_relative_path;
    const std::string focus = g_pending_delete_refresh.focus_relative_path;
    const std::string hidden = g_pending_delete_refresh.deleted_relative_path;
    const std::string normalized_directory =
        switchbox::core::smb_join_relative_path({}, directory);
    const std::string normalized_current =
        switchbox::core::smb_join_relative_path({}, this->relative_path);

    g_pending_delete_refresh.pending = false;
    g_pending_delete_refresh.kind = PendingRefreshKind::None;
    g_pending_delete_refresh.deleted_relative_path.clear();

    if (kind == PendingRefreshKind::DeleteAfterPlayerExit) {
        std::string error_message;
        if (!switchbox::core::delete_smb_file(this->source, hidden, error_message)) {
            if (error_message.empty()) {
                error_message = "Failed to delete SMB file.";
            }
            auto* failed = new brls::Dialog(error_message);
            failed->open();
            return true;
        }

        if (normalized_directory == normalized_current && this->has_cached_entries) {
            this->apply_local_delete_result(hidden, focus);
            return true;
        }

        this->refresh_after_player_delete(normalized_directory, focus, hidden);
        return true;
    }

    if (normalized_directory == normalized_current && this->has_cached_entries) {
        if (!this->restore_focus_now(focus)) {
            this->schedule_focus_restore(focus);
        }
        return true;
    }

    this->refresh_after_player_delete(normalized_directory, focus);
    return true;
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

    if (auto* current_focus = brls::Application::getCurrentFocus();
        current_focus != nullptr && current_focus->getParentActivity() == this) {
        brls::Application::giveFocus(nullptr);
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

    const std::string normalized_focus =
        switchbox::core::smb_join_relative_path({}, focus_relative_path);
    if (!restore_focus_now(normalized_focus)) {
        schedule_focus_restore(normalized_focus);
    }
}

}  // namespace switchbox::app
