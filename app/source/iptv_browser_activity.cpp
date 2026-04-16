#include "switchbox/app/iptv_browser_activity.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <borealis/core/application.hpp>
#include <borealis/core/box.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>
#include <borealis/views/sidebar.hpp>

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

std::string build_error_dialog_message(
    const std::string& summary,
    const std::string& detail) {
    if (detail.empty()) {
        return summary;
    }

    return summary + "\n\n" + detail;
}

std::string visible_iptv_title(const switchbox::core::IptvSourceSettings& source) {
    if (!source.title.empty()) {
        return source.title;
    }

    return tr("home/cards/common/untitled_iptv");
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

void apply_header_subtitle(
    brls::AppletFrame* frame,
    const std::string& text,
    NVGcolor color) {
    if (text.empty()) {
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

    auto* subtitle = create_label(text, 13.0f, color, true);
    subtitle->setMargins(0, 0, 0, title_font_size);
    left_header_box->addView(subtitle);
}

struct IptvContentBuild {
    brls::View* view = nullptr;
    brls::View* focus_view = nullptr;
};

IptvContentBuild create_loading_content(const switchbox::core::IptvSourceSettings& source) {
    auto theme = brls::Application::getTheme();

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(18, 40, 30, 40);

    auto* message = create_focusable_message_label(
        tr("iptv_browser/loading"),
        18.0f,
        theme["brls/text"]);
    message->setMargins(0, 10, 10, 0);
    content->addView(message);

    auto* detail = create_label(
        tr("iptv_browser/loading_detail"),
        15.0f,
        theme["brls/text_disabled"]);
    detail->setMargins(0, 0, 14, 0);
    content->addView(detail);

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(content);

    auto* frame = new brls::AppletFrame(scrolling_frame);
    frame->setTitle(visible_iptv_title(source));
    apply_header_subtitle(frame, source.url, theme["brls/text_disabled"]);
    return {
        .view = frame,
        .focus_view = message,
    };
}

bool view_is_descendant_of(brls::View* view, brls::View* ancestor) {
    while (view != nullptr) {
        if (view == ancestor) {
            return true;
        }

        view = view->getParent();
    }

    return false;
}

std::string detect_stream_kind(const switchbox::core::IptvPlaylistEntry& entry) {
    const std::string& url = entry.stream_url;
    if (url.find("youtube.com") != std::string::npos || url.find("youtu.be") != std::string::npos) {
        return "YouTube";
    }

    if (url.find("twitch.tv") != std::string::npos) {
        return "Twitch";
    }

    const auto lower_url = [&]() {
        std::string value = url;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }();

    if (lower_url.find(".m3u8") != std::string::npos) {
        return "HLS";
    }

    if (lower_url.find(".mpd") != std::string::npos) {
        return "DASH";
    }

    if (lower_url.starts_with("rtmp://")) {
        return "RTMP";
    }

    if (lower_url.starts_with("http://") || lower_url.starts_with("https://")) {
        return "HTTP";
    }

    return {};
}

std::string join_detail_parts(const std::vector<std::string>& parts) {
    std::string detail;
    for (const auto& part : parts) {
        if (part.empty()) {
            continue;
        }

        if (!detail.empty()) {
            detail += " · ";
        }

        detail += part;
    }

    return detail;
}

}  // namespace

IptvBrowserActivity::IptvBrowserActivity(switchbox::core::IptvSourceSettings source)
    : brls::Activity(create_loading_content(source).view)
    , source(std::move(source)) {
}

IptvBrowserActivity::~IptvBrowserActivity() {
    this->load_cancelled->store(true);
}

void IptvBrowserActivity::onContentAvailable() {
    brls::Activity::onContentAvailable();
    install_common_actions();
    start_loading_if_needed();
}

void IptvBrowserActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    install_common_actions();
    start_loading_if_needed();
}

void IptvBrowserActivity::install_common_actions() {
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
        [this](brls::View*) {
            return handle_back_action();
        });
    this->action_home_id = registerAction(
        tr("actions/home"),
        brls::BUTTON_Y,
        [this](brls::View*) {
            return_to_home();
            return true;
        });
}

void IptvBrowserActivity::start_loading_if_needed() {
    if (this->load_started) {
        return;
    }

    this->load_started = true;
    auto cancel_flag = this->load_cancelled;
    const auto source_copy = this->source;
    brls::async([this, cancel_flag, source_copy]() {
        switchbox::core::IptvPlaylistResult result;
        try {
            result = switchbox::core::load_iptv_playlist(source_copy);
        } catch (const std::exception& exception) {
            result.backend_available = true;
            result.success = false;
            result.error_message = exception.what();
        } catch (...) {
            result.backend_available = true;
            result.success = false;
            result.error_message = tr("iptv_browser/unexpected_error");
        }

        brls::sync([this, cancel_flag, result = std::move(result)]() mutable {
            if (cancel_flag->load()) {
                return;
            }

            try {
                apply_playlist_result(std::move(result));
            } catch (const std::exception& exception) {
                show_error_and_return_home(build_error_dialog_message(
                    tr("iptv_browser/open_failed"),
                    exception.what()));
            } catch (...) {
                show_error_and_return_home(build_error_dialog_message(
                    tr("iptv_browser/open_failed"),
                    tr("iptv_browser/unexpected_error")));
            }
        });
    });
}

void IptvBrowserActivity::apply_playlist_result(switchbox::core::IptvPlaylistResult result) {
    this->playlist_result = std::move(result);

    if (!this->playlist_result.backend_available) {
        show_error_and_return_home(tr("iptv_browser/backend_unavailable"));
        return;
    }

    if (!this->playlist_result.success) {
        show_error_and_return_home(build_error_dialog_message(
            tr("iptv_browser/open_failed"),
            this->playlist_result.error_message));
        return;
    }

    if (this->playlist_result.entries.empty()) {
        show_error_and_return_home(build_error_dialog_message(
            tr("iptv_browser/open_failed"),
            tr("iptv_browser/empty")));
        return;
    }

    build_group_model();
    build_grouped_ui();
    install_common_actions();
    focus_sidebar();
}

void IptvBrowserActivity::show_error_and_return_home(const std::string& message) {
    this->grouped_ui_ready = false;
    this->sidebar = nullptr;
    this->right_content_box = nullptr;
    this->right_scrolling_frame = nullptr;

    auto* dialog = new brls::Dialog(message);
    dialog->addButton(
        brls::getStr("hints/ok"),
        [this]() {
            return_to_home();
        });
    dialog->open();
}

void IptvBrowserActivity::build_group_model() {
    this->groups.clear();
    this->groups.push_back({
        .title = tr("iptv_browser/groups/favorites"),
        .entry_indices = {},
        .favorites = true,
    });

    for (size_t index = 0; index < this->playlist_result.entries.size(); ++index) {
        const auto& entry = this->playlist_result.entries[index];
        const std::string group_title =
            entry.group_title.empty() ? tr("iptv_browser/groups/ungrouped") : entry.group_title;

        auto existing = std::find_if(
            this->groups.begin() + 1,
            this->groups.end(),
            [&group_title](const PlaylistGroup& group) {
                return group.title == group_title && !group.favorites;
            });

        if (existing == this->groups.end()) {
            this->groups.push_back({
                .title = group_title,
                .entry_indices = {index},
                .favorites = false,
            });
            continue;
        }

        existing->entry_indices.push_back(index);
    }

    if (this->selected_group_index >= this->groups.size()) {
        this->selected_group_index = 0;
    }
}

void IptvBrowserActivity::build_grouped_ui() {
    auto* root = new brls::Box(brls::Axis::ROW);

    auto* sidebar = new brls::Sidebar();
    sidebar->setWidth(280);
    sidebar->setGrow(0);
    this->sidebar = sidebar;

    for (size_t index = 0; index < this->groups.size(); ++index) {
        sidebar->addItem(
            this->groups[index].title,
            [this, index](brls::View*) {
                select_group(index);
            });
    }
    root->addView(sidebar);

    auto* right_content = new brls::Box(brls::Axis::COLUMN);
    right_content->setPadding(24, 40, 24, 40);
    this->right_content_box = right_content;

    auto* right_frame = new brls::ScrollingFrame();
    right_frame->setContentView(right_content);
    right_frame->setGrow(1.0f);
    this->right_scrolling_frame = right_frame;
    root->addView(right_frame);

    auto* frame = new brls::AppletFrame(root);
    frame->setTitle(visible_iptv_title(this->source));
    apply_header_subtitle(frame, this->source.url, brls::Application::getTheme()["brls/text_disabled"]);
    setContentView(frame);

    this->grouped_ui_ready = true;
    rebuild_right_panel();
}

void IptvBrowserActivity::rebuild_right_panel(
    const std::string& preferred_entry_key,
    size_t fallback_index,
    bool request_focus) {
    if (!this->grouped_ui_ready || this->right_content_box == nullptr) {
        return;
    }

    auto theme = brls::Application::getTheme();
    this->right_content_box->clearViews();

    const auto entries = current_group_entry_indices();
    const auto& group = this->groups[this->selected_group_index];

    auto* title_label = create_label(group.title, 20.0f, theme["brls/text"], true);
    title_label->setMargins(0, 0, 8, 0);
    this->right_content_box->addView(title_label);

    auto* count_label = create_label(
        tr("iptv_browser/group_count", std::to_string(entries.size())),
        15.0f,
        theme["brls/text_disabled"],
        true);
    count_label->setMargins(0, 0, 16, 0);
    this->right_content_box->addView(count_label);

    brls::View* focus_target = nullptr;
    brls::View* fallback_target = nullptr;
    brls::View* first_target = nullptr;

    if (entries.empty()) {
        const std::string empty_key =
            group.favorites ? "iptv_browser/favorites_empty" : "iptv_browser/group_empty";
        auto* empty_label = create_focusable_message_label(
            tr(empty_key),
            17.0f,
            theme["brls/text_disabled"]);
        empty_label->setMargins(0, 8, 8, 0);
        this->right_content_box->addView(empty_label);
        focus_target = empty_label;
    } else {
        for (size_t position = 0; position < entries.size(); ++position) {
            const auto& entry = this->playlist_result.entries[entries[position]];
            std::vector<std::string> detail_parts;
            if (is_favorite_entry(entry)) {
                detail_parts.push_back(tr("iptv_browser/channel/favorite_badge"));
            }
            if (!entry.tvg_chno.empty()) {
                detail_parts.push_back(tr("iptv_browser/channel/number_value", entry.tvg_chno));
            }
            if (!entry.tvg_country.empty()) {
                detail_parts.push_back(entry.tvg_country);
            }
            const std::string stream_kind = detect_stream_kind(entry);
            if (!stream_kind.empty()) {
                detail_parts.push_back(stream_kind);
            }

            auto* cell = create_detail_cell(
                entry.title.empty() ? tr("player_page/fallback_title") : entry.title,
                join_detail_parts(detail_parts),
                theme["brls/list/listItem_value_color"]);
            cell->registerClickAction([this, entry](brls::View*) {
                brls::Application::pushActivity(new PlayerActivity(
                    switchbox::core::make_iptv_playback_target(this->source, entry)));
                return true;
            });
            cell->registerAction(
                is_favorite_entry(entry) ? tr("iptv_browser/actions/unfavorite")
                                         : tr("iptv_browser/actions/favorite"),
                brls::BUTTON_X,
                [this, entry, position](brls::View*) {
                    return toggle_favorite_for_entry(entry, position);
                },
                false,
                false,
                brls::SOUND_CLICK);
            this->right_content_box->addView(cell);

            if (first_target == nullptr) {
                first_target = cell;
            }
            if (position == fallback_index) {
                fallback_target = cell;
            }
            if (!preferred_entry_key.empty() && entry.favorite_key == preferred_entry_key) {
                focus_target = cell;
            }
        }
    }

    if (focus_target == nullptr) {
        focus_target = fallback_target != nullptr ? fallback_target : first_target;
    }

    if (request_focus) {
        if (focus_target != nullptr) {
            brls::Application::giveFocus(focus_target);
        } else {
            focus_sidebar();
        }
    }
}

void IptvBrowserActivity::select_group(size_t group_index) {
    if (!this->grouped_ui_ready || group_index >= this->groups.size()) {
        return;
    }

    this->selected_group_index = group_index;
    if (this->right_scrolling_frame != nullptr) {
        this->right_scrolling_frame->setContentOffsetY(0.0f, false);
    }
    rebuild_right_panel();
}

bool IptvBrowserActivity::handle_back_action() {
    if (!this->grouped_ui_ready) {
        brls::Application::popActivity(brls::TransitionAnimation::FADE);
        return true;
    }

    if (is_focus_in_right_panel()) {
        focus_sidebar();
        return true;
    }

    brls::Application::popActivity(brls::TransitionAnimation::FADE);
    return true;
}

bool IptvBrowserActivity::is_focus_in_right_panel() const {
    if (this->right_scrolling_frame == nullptr || this->right_content_box == nullptr) {
        return false;
    }

    brls::View* focus = brls::Application::getCurrentFocus();
    return focus != nullptr &&
           (view_is_descendant_of(focus, this->right_content_box) ||
            view_is_descendant_of(focus, this->right_scrolling_frame));
}

void IptvBrowserActivity::focus_sidebar() {
    if (!this->grouped_ui_ready || this->sidebar == nullptr || this->groups.empty()) {
        return;
    }

    brls::Application::giveFocus(this->sidebar->getItem(static_cast<int>(this->selected_group_index)));
}

std::vector<size_t> IptvBrowserActivity::current_group_entry_indices() const {
    if (this->groups.empty() || this->selected_group_index >= this->groups.size()) {
        return {};
    }

    const auto& group = this->groups[this->selected_group_index];
    if (!group.favorites) {
        return group.entry_indices;
    }

    std::unordered_set<std::string> favorite_keys(
        this->source.favorite_keys.begin(),
        this->source.favorite_keys.end());
    std::vector<size_t> indices;
    indices.reserve(this->playlist_result.entries.size());

    for (size_t index = 0; index < this->playlist_result.entries.size(); ++index) {
        if (favorite_keys.contains(this->playlist_result.entries[index].favorite_key)) {
            indices.push_back(index);
        }
    }

    return indices;
}

bool IptvBrowserActivity::is_favorite_entry(const switchbox::core::IptvPlaylistEntry& entry) const {
    return std::find(
               this->source.favorite_keys.begin(),
               this->source.favorite_keys.end(),
               entry.favorite_key) != this->source.favorite_keys.end();
}

bool IptvBrowserActivity::persist_favorite_keys(const std::vector<std::string>& favorite_keys) {
    const auto old_local_keys = this->source.favorite_keys;
    this->source.favorite_keys = favorite_keys;

    auto& config = switchbox::core::AppConfigStore::mutable_config();
    auto config_it = std::find_if(
        config.iptv_sources.begin(),
        config.iptv_sources.end(),
        [this](const switchbox::core::IptvSourceSettings& candidate) {
            return candidate.key == this->source.key;
        });

    std::vector<std::string> old_config_keys;
    if (config_it != config.iptv_sources.end()) {
        old_config_keys = config_it->favorite_keys;
        config_it->favorite_keys = favorite_keys;
    }

    if (switchbox::core::AppConfigStore::save()) {
        return true;
    }

    this->source.favorite_keys = old_local_keys;
    if (config_it != config.iptv_sources.end()) {
        config_it->favorite_keys = old_config_keys;
    }
    return false;
}

bool IptvBrowserActivity::toggle_favorite_for_entry(
    const switchbox::core::IptvPlaylistEntry& entry,
    size_t fallback_index) {
    std::vector<std::string> updated_keys = this->source.favorite_keys;
    const auto existing = std::find(updated_keys.begin(), updated_keys.end(), entry.favorite_key);
    const bool removing = existing != updated_keys.end();

    if (removing) {
        updated_keys.erase(existing);
    } else {
        updated_keys.push_back(entry.favorite_key);
    }

    if (!persist_favorite_keys(updated_keys)) {
        brls::Application::notify(tr("iptv_browser/favorite_save_failed"));
        return true;
    }

    brls::Application::notify(
        tr(removing ? "iptv_browser/favorite_removed" : "iptv_browser/favorite_added", entry.title));

    const std::string preferred_key = removing ? std::string{} : entry.favorite_key;
    rebuild_right_panel(preferred_key, fallback_index, true);
    return true;
}

void IptvBrowserActivity::return_to_home() {
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
