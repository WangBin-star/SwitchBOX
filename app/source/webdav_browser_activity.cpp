#include "switchbox/app/webdav_browser_activity.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <borealis/core/application.hpp>
#include <borealis/core/box.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/bottom_bar.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/player_activity.hpp"
#include "switchbox/core/playback_target.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::string visible_webdav_title(const switchbox::core::WebDavSourceSettings& source) {
    if (!source.title.empty()) {
        return source.title;
    }

    return tr("home/cards/common/untitled_webdav");
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

void attach_directory_icon_prefix(brls::DetailCell* cell, NVGcolor icon_color) {
    if (cell == nullptr || cell->title == nullptr) {
        return;
    }

    auto* icon_label = create_label("\uE2C7", cell->title->getFontSize(), icon_color, true);
    icon_label->setLineHeight(cell->title->getLineHeight());
    const float title_height = cell->title->getFontSize() * cell->title->getLineHeight();
    const float icon_down_offset = std::max(1.0f, title_height / 6.0f);
    icon_label->setMargins(icon_down_offset, 8.0f, 0.0f, 0.0f);
    cell->addView(icon_label, 0);
}

std::string make_entry_view_id(const std::string& relative_path) {
    return "switchbox/webdav_entry/" + switchbox::core::webdav_join_relative_path({}, relative_path);
}

struct WebDavBrowserContentBuild {
    brls::View* view = nullptr;
    std::vector<switchbox::core::WebDavBrowserEntry> rendered_entries;
};

WebDavBrowserContentBuild build_webdav_browser_content_from_result(
    const switchbox::core::WebDavSourceSettings& source,
    const std::string& relative_path,
    const switchbox::core::WebDavBrowserResult& browse_result) {
    auto theme = brls::Application::getTheme();
    const auto& config = switchbox::core::AppConfigStore::current();

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(18, 40, 30, 40);

    if (!browse_result.backend_available) {
        auto* message = create_focusable_message_label(
            tr("webdav_browser/backend_unavailable"),
            18.0f,
            theme["brls/text"]);
        message->setMargins(0, 10, 10, 0);
        content->addView(message);
    } else if (!browse_result.success) {
        auto* message = create_focusable_message_label(
            tr("webdav_browser/open_failed"),
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
    } else if (browse_result.entries.empty()) {
        auto* empty_label = create_focusable_message_label(
            tr("webdav_browser/empty"),
            17.0f,
            theme["brls/text_disabled"]);
        empty_label->setMargins(8, 10, 14, 14);
        content->addView(empty_label);
    } else {
        for (size_t entry_index = 0; entry_index < browse_result.entries.size(); ++entry_index) {
            const auto& entry = browse_result.entries[entry_index];
            const std::string detail = entry.is_directory ? tr("webdav_browser/folder_detail")
                                                          : tr(
                                                                "webdav_browser/file_detail",
                                                                format_bytes(entry.size));
            auto* cell = create_detail_cell(
                entry.name,
                detail,
                entry.is_directory ? theme["brls/highlight/color2"]
                                   : theme["brls/list/listItem_value_color"]);
            if (entry.is_directory) {
                attach_directory_icon_prefix(cell, theme["brls/highlight/color2"]);
            }

            cell->setId(make_entry_view_id(entry.relative_path));
            cell->registerClickAction([source, entry](brls::View*) {
                if (entry.is_directory) {
                    brls::Application::pushActivity(new WebDavBrowserActivity(source, entry.relative_path));
                    return true;
                }

                brls::Application::pushActivity(new PlayerActivity(
                    switchbox::core::make_webdav_playback_target(source, entry.relative_path)));
                return true;
            });
            content->addView(cell);

            if (entry_index == 0) {
                content->setDefaultFocusedIndex(0);
            }
        }
    }

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(content);

    auto* frame = new brls::AppletFrame(scrolling_frame);
    apply_native_status_layout(frame, visible_webdav_title(source));
    apply_header_path(
        frame,
        switchbox::core::webdav_display_path(source, relative_path),
        theme["brls/text_disabled"]);
    apply_footer_extensions_hint(
        frame,
        tr("webdav_browser/extensions_hint", config.general.playable_extensions),
        theme["brls/text_disabled"]);
    return {
        .view = frame,
        .rendered_entries = browse_result.success ? browse_result.entries : std::vector<switchbox::core::WebDavBrowserEntry>{},
    };
}

WebDavBrowserContentBuild build_webdav_browser_content(
    const switchbox::core::WebDavSourceSettings& source,
    const std::string& relative_path) {
    const auto& config = switchbox::core::AppConfigStore::current();
    const auto browse_result =
        switchbox::core::browse_webdav_directory(source, config.general, relative_path);
    return build_webdav_browser_content_from_result(source, relative_path, browse_result);
}

}  // namespace

WebDavBrowserActivity::WebDavBrowserActivity(
    switchbox::core::WebDavSourceSettings source,
    std::string relative_path)
    : brls::Activity()
    , source(std::move(source))
    , relative_path(std::move(relative_path)) {
}

WebDavBrowserActivity::WebDavBrowserActivity(
    switchbox::core::WebDavSourceSettings source,
    switchbox::core::WebDavBrowserResult preloaded_result)
    : brls::Activity()
    , source(std::move(source))
    , relative_path(preloaded_result.requested_path)
    , preloaded_result(std::move(preloaded_result))
    , has_preloaded_result(true) {
}

WebDavBrowserActivity::~WebDavBrowserActivity() = default;

brls::View* WebDavBrowserActivity::createContentView() {
    const auto build = this->has_preloaded_result
                           ? build_webdav_browser_content_from_result(
                                 this->source,
                                 this->relative_path,
                                 this->preloaded_result)
                           : build_webdav_browser_content(this->source, this->relative_path);
    this->has_preloaded_result = false;
    this->cached_entries = build.rendered_entries;
    this->has_cached_entries = !this->cached_entries.empty();
    return build.view;
}

void WebDavBrowserActivity::onContentAvailable() {
    brls::Activity::onContentAvailable();
    install_common_actions();
    bind_entry_focus_tracking();
}

void WebDavBrowserActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    install_common_actions();
    sync_focused_entry_from_ui();
}

void WebDavBrowserActivity::install_common_actions() {
    if (this->action_back_id != ACTION_NONE) {
        unregisterAction(this->action_back_id);
        this->action_back_id = ACTION_NONE;
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
    this->action_home_id = registerAction(
        tr("actions/home"),
        brls::BUTTON_Y,
        [this](brls::View*) {
            return_to_home();
            return true;
        });
}

void WebDavBrowserActivity::bind_entry_focus_tracking() {
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
                switchbox::core::webdav_join_relative_path({}, relative_path);
            this->focused_entry_is_directory = is_directory;
        });
    }

    this->focused_entry_relative_path =
        switchbox::core::webdav_join_relative_path({}, this->cached_entries.front().relative_path);
    this->focused_entry_is_directory = this->cached_entries.front().is_directory;
}

void WebDavBrowserActivity::sync_focused_entry_from_ui() {
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
                    switchbox::core::webdav_join_relative_path({}, entry.relative_path);
                this->focused_entry_is_directory = entry.is_directory;
                return;
            }
        }
    }
}

void WebDavBrowserActivity::return_to_home() {
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

}  // namespace switchbox::app
