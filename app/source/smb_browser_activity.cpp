#include "switchbox/app/smb_browser_activity.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

#include <borealis/core/box.hpp>
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

struct SmbBrowserContentBuild {
    brls::View* view = nullptr;
    brls::View* focus_view = nullptr;
};

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
    const std::string& preferred_focus_relative_path = {}) {
    auto theme = brls::Application::getTheme();
    const auto& config = switchbox::core::AppConfigStore::current();
    const auto browse_result =
        switchbox::core::browse_smb_directory(source, config.general, relative_path);

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(18, 40, 30, 40);

    if (!browse_result.backend_available) {
        auto* message = create_label(
            tr("smb_browser/backend_unavailable"),
            18.0f,
            theme["brls/text"]);
        message->setMargins(0, 10, 10, 0);
        content->addView(message);
    } else if (!browse_result.success) {
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
    } else {
        brls::View* focus_view = nullptr;
        if (browse_result.entries.empty()) {
            auto* empty_label = create_label(
                tr("smb_browser/empty"),
                17.0f,
                theme["brls/text_disabled"]);
            empty_label->setMargins(8, 10, 14, 14);
            content->addView(empty_label);
        }

        for (const auto& entry : browse_result.entries) {
            const std::string title =
                entry.is_directory ? tr("smb_browser/folder_prefix", entry.name)
                                   : tr("smb_browser/file_prefix", entry.name);
            const std::string detail = entry.is_directory ? tr("smb_browser/folder_detail")
                                                          : tr(
                                                                "smb_browser/file_detail",
                                                                format_bytes(entry.size));

            auto* cell = create_detail_cell(
                title,
                detail,
                entry.is_directory ? theme["brls/highlight/color2"]
                                   : theme["brls/list/listItem_value_color"]);
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
            }
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
        };
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
        .focus_view = nullptr,
    };
}

}  // namespace

void SmbBrowserActivity::request_focus_after_return(
    const switchbox::core::SmbSourceSettings& source,
    std::string directory_relative_path,
    std::string focus_relative_path) {
    (void)source;
    (void)directory_relative_path;
    (void)focus_relative_path;
}

SmbBrowserActivity::SmbBrowserActivity(
    switchbox::core::SmbSourceSettings source,
    std::string relative_path)
    : brls::Activity(create_smb_browser_content(source, relative_path).view)
    , source(std::move(source))
    , relative_path(std::move(relative_path)) {
    registerExitAction();
}

void SmbBrowserActivity::refresh_after_player_delete(
    const std::string& directory_relative_path,
    const std::string& focus_relative_path) {
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
    auto rebuilt = create_smb_browser_content(this->source, this->relative_path, focus_relative_path);
    setContentView(rebuilt.view);

    if (auto* scrolling = find_scrolling_frame_from_root(this->getContentView())) {
        scrolling->setContentOffsetY(keep_scroll ? previous_scroll_offset : 0.0f, false);
    }

    if (rebuilt.focus_view != nullptr) {
        brls::Application::giveFocus(rebuilt.focus_view);
    }
}

}  // namespace switchbox::app
