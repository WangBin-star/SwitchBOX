#include "switchbox/app/iptv_browser_activity.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
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
#include <borealis/views/recycler.hpp>
#include <borealis/views/scrolling_frame.hpp>
#include <borealis/views/sidebar.hpp>

#include "switchbox/app/player_activity.hpp"
#include "switchbox/core/playback_target.hpp"

namespace switchbox::app {

namespace {

struct IptvDebugLogState {
    bool initialized = false;
    uint64_t sequence = 0;
    uint64_t session_token = 0;
    std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::time_point::min();
};

IptvDebugLogState& iptv_debug_log_state() {
    static IptvDebugLogState state;
    return state;
}

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
}

std::filesystem::path iptv_debug_log_path() {
    return switchbox::core::AppConfigStore::paths().base_directory / "iptv_debug.log";
}

uint64_t make_iptv_debug_session_token() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto token = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return token == 0 ? 1 : token;
}

void reset_iptv_debug_log() {
    std::error_code error;
    const auto log_path = iptv_debug_log_path();
    std::filesystem::create_directories(
        switchbox::core::AppConfigStore::paths().base_directory,
        error);

    std::ofstream output(log_path, std::ios::binary | std::ios::out | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    auto& state = iptv_debug_log_state();
    state.initialized = true;
    state.session_start = std::chrono::steady_clock::now();
    state.session_token = make_iptv_debug_session_token();

    output << "\n========== IPTV LOG SESSION BEGIN ==========\n";
    output << "[t+0ms][#" << (++state.sequence) << "] [iptv] log_format=2026-04-17f\n";
    output << "[t+0ms][#" << (++state.sequence) << "] [iptv] session_token=" << state.session_token << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [iptv] base_directory="
           << switchbox::core::AppConfigStore::paths().base_directory.string()
           << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [iptv] config_file="
           << switchbox::core::AppConfigStore::paths().config_file.string()
           << '\n';
    output.flush();
}

void append_iptv_debug_log(const std::string& message) {
    auto& state = iptv_debug_log_state();
    if (!state.initialized) {
        reset_iptv_debug_log();
    }

    std::ofstream output(iptv_debug_log_path(), std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (state.session_start == std::chrono::steady_clock::time_point::min()) {
        state.session_start = now;
    }

    std::string sanitized = message;
    for (char& character : sanitized) {
        if (character == '\r' || character == '\n') {
            character = ' ';
        }
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.session_start).count();
    output << "[t+" << elapsed_ms << "ms]"
           << "[#" << (++state.sequence) << "] "
           << sanitized
           << '\n';
}

std::string sanitize_iptv_log_text(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n') {
            character = ' ';
        }
    }
    return value;
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

class IptvEntryCell : public brls::DetailCell {
public:
    IptvEntryCell() {
        this->setHeight(brls::Application::getStyle()["brls/dropdown/listItemHeight"]);
        if (this->title != nullptr) {
            this->title->setSingleLine(true);
        }
        if (this->detail != nullptr) {
            this->detail->setSingleLine(true);
        }

        this->favorite_action_id = this->registerAction(
            tr("iptv_browser/actions/favorite"),
            brls::BUTTON_X,
            [this](brls::View*) {
                return this->favorite_listener ? this->favorite_listener() : false;
            },
            false,
            false,
            brls::SOUND_CLICK);
    }

    void bind(
        const std::string& title,
        const std::string& detail,
        const std::string& favorite_hint,
        std::function<bool()> favorite_listener) {
        this->setText(title);
        this->setDetailText(detail);
        this->favorite_listener = std::move(favorite_listener);
        this->updateActionHint(brls::BUTTON_X, favorite_hint);
        this->setActionAvailable(brls::BUTTON_X, this->favorite_listener != nullptr);
    }

    void prepareForReuse() override {
        brls::DetailCell::prepareForReuse();
        this->favorite_listener = nullptr;
    }

private:
    brls::ActionIdentifier favorite_action_id = ACTION_NONE;
    std::function<bool()> favorite_listener;
};

IptvContentBuild create_loading_content(const switchbox::core::IptvSourceSettings& source) {
    auto theme = brls::Application::getTheme();

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(18, 40, 30, 40);

    auto* message = create_label(
        tr("iptv_browser/loading"),
        18.0f,
        theme["brls/text"],
        false);
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
        .focus_view = nullptr,
    };
}

}  // namespace

void begin_iptv_debug_log_session() {
    reset_iptv_debug_log();
    append_iptv_debug_log("[iptv] startup");
}

class IptvBrowserDataSource : public brls::RecyclerDataSource {
public:
    explicit IptvBrowserDataSource(IptvBrowserActivity* activity)
        : activity(activity) {}

    int numberOfRows(brls::RecyclerFrame* recycler, int section) override {
        (void)recycler;
        (void)section;
        return static_cast<int>(this->activity->visible_entry_indices.size());
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = static_cast<IptvEntryCell*>(recycler->dequeueReusableCell("switchbox::iptv_entry"));
        const size_t position = static_cast<size_t>(index.row);
        const size_t entry_index = this->activity->visible_entry_indices[position];
        const auto& entry = this->activity->playlist_result.entries[entry_index];
        const auto detect_stream_kind_for_entry = [](const switchbox::core::IptvPlaylistEntry& current_entry) {
            const std::string& url = current_entry.stream_url;
            if (url.find("youtube.com") != std::string::npos || url.find("youtu.be") != std::string::npos) {
                return std::string("YouTube");
            }

            if (url.find("twitch.tv") != std::string::npos) {
                return std::string("Twitch");
            }

            std::string lower_url = url;
            std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (lower_url.find(".m3u8") != std::string::npos) {
                return std::string("HLS");
            }
            if (lower_url.find(".mpd") != std::string::npos) {
                return std::string("DASH");
            }
            if (lower_url.starts_with("rtmp://")) {
                return std::string("RTMP");
            }
            if (lower_url.starts_with("http://") || lower_url.starts_with("https://")) {
                return std::string("HTTP");
            }

            return std::string {};
        };
        const auto join_detail_parts_for_entry = [](const std::vector<std::string>& parts) {
            std::string detail;
            for (const auto& part : parts) {
                if (part.empty()) {
                    continue;
                }
                if (!detail.empty()) {
                    detail += " | ";
                }
                detail += part;
            }
            return detail;
        };

        std::vector<std::string> detail_parts;
        if (this->activity->is_favorite_entry(entry)) {
            detail_parts.push_back(tr("iptv_browser/channel/favorite_badge"));
        }
        if (!entry.tvg_chno.empty()) {
            detail_parts.push_back(tr("iptv_browser/channel/number_value", entry.tvg_chno));
        }
        if (!entry.tvg_country.empty()) {
            detail_parts.push_back(entry.tvg_country);
        }
        const std::string stream_kind = detect_stream_kind_for_entry(entry);
        if (!stream_kind.empty()) {
            detail_parts.push_back(stream_kind);
        }

        cell->bind(
            entry.title.empty() ? tr("player_page/fallback_title") : entry.title,
            join_detail_parts_for_entry(detail_parts),
            this->activity->is_favorite_entry(entry) ? tr("iptv_browser/actions/unfavorite")
                                                     : tr("iptv_browser/actions/favorite"),
            [activity = this->activity, entry, position]() {
                return activity->toggle_favorite_for_entry(entry, position);
            });
        return cell;
    }

    float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        (void)recycler;
        (void)index;
        return brls::Application::getStyle()["brls/dropdown/listItemHeight"];
    }

    void didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        (void)recycler;
        const size_t position = static_cast<size_t>(index.row);
        if (position >= this->activity->visible_entry_indices.size()) {
            return;
        }

        const size_t entry_index = this->activity->visible_entry_indices[position];
        const auto& entry = this->activity->playlist_result.entries[entry_index];
        append_iptv_debug_log(
            "[iptv] launch_player source_title=" + sanitize_iptv_log_text(visible_iptv_title(this->activity->source)) +
            " group_index=" + std::to_string(this->activity->selected_group_index) +
            " group_title=" + sanitize_iptv_log_text(entry.group_title) +
            " entry_index=" + std::to_string(entry_index) +
            " row=" + std::to_string(position) +
            " entry_title=" + sanitize_iptv_log_text(entry.title) +
            " entry_url=" + sanitize_iptv_log_text(entry.stream_url));
        brls::Application::pushActivity(new PlayerActivity(
            switchbox::core::make_iptv_playback_target(this->activity->source, entry)));
    }

private:
    IptvBrowserActivity* activity = nullptr;
};

namespace {

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
            detail += " 路 ";
        }

        detail += part;
    }

    return detail;
}

}  // namespace

IptvBrowserActivity::IptvBrowserActivity(switchbox::core::IptvSourceSettings source)
    : brls::Activity(create_loading_content(source).view)
    , source(std::move(source)) {
    auto& state = iptv_debug_log_state();
    if (!state.initialized) {
        reset_iptv_debug_log();
    }
    state.session_start = std::chrono::steady_clock::now();
    state.session_token = make_iptv_debug_session_token();
    append_iptv_debug_log("[iptv] activity_session_begin token=" + std::to_string(state.session_token));
    append_iptv_debug_log("[iptv] activity created");
    append_iptv_debug_log("[iptv] source key=" + this->source.key);
    append_iptv_debug_log("[iptv] source title=" + visible_iptv_title(this->source));
    append_iptv_debug_log("[iptv] source url=" + this->source.url);
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

void IptvBrowserActivity::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    this->load_cancelled->store(true);
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
    if (this->action_nav_up_id != ACTION_NONE) {
        unregisterAction(this->action_nav_up_id);
        this->action_nav_up_id = ACTION_NONE;
    }
    if (this->action_nav_down_id != ACTION_NONE) {
        unregisterAction(this->action_nav_down_id);
        this->action_nav_down_id = ACTION_NONE;
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
            if (!this->grouped_ui_ready) {
                append_iptv_debug_log("[iptv] home_action direct_home_loading");
                this->load_cancelled->store(true);
                return_to_home();
                return true;
            }

            append_iptv_debug_log("[iptv] home_action confirm");
            show_exit_confirm_dialog();
            return true;
        });
    this->action_nav_up_id = registerAction(
        "",
        brls::BUTTON_NAV_UP,
        [this](brls::View*) {
            return this->move_right_panel_focus(-1);
        },
        true,
        true);
    this->action_nav_down_id = registerAction(
        "",
        brls::BUTTON_NAV_DOWN,
        [this](brls::View*) {
            return this->move_right_panel_focus(1);
        },
        true,
        true);
}

void IptvBrowserActivity::refresh_source_from_config_if_available() {
    if (this->source.key.empty()) {
        return;
    }

    const auto& config = switchbox::core::AppConfigStore::current();
    const auto it = std::find_if(
        config.iptv_sources.begin(),
        config.iptv_sources.end(),
        [this](const switchbox::core::IptvSourceSettings& candidate) {
            return candidate.key == this->source.key;
        });
    if (it != config.iptv_sources.end()) {
        this->source = *it;
    }
}

void IptvBrowserActivity::start_loading_if_needed() {
    refresh_source_from_config_if_available();
    if (this->load_started) {
        return;
    }

    this->load_started = true;
    append_iptv_debug_log("[iptv] start_loading_if_needed");
    auto cancel_flag = this->load_cancelled;
    const auto source_copy = this->source;
    brls::async([this, cancel_flag, source_copy]() {
        auto result = std::make_shared<switchbox::core::IptvPlaylistResult>();
        try {
            append_iptv_debug_log("[iptv] background load begin");
            *result = switchbox::core::load_iptv_playlist(source_copy, cancel_flag);
            append_iptv_debug_log(
                "[iptv] background load end success=" +
                std::string(result->success ? "true" : "false") +
                " backend=" +
                std::string(result->backend_available ? "true" : "false") +
                " entries=" +
                std::to_string(result->entries.size()));
            if (!result->error_message.empty()) {
                append_iptv_debug_log("[iptv] load error=" + result->error_message);
            }
        } catch (const std::exception& exception) {
            result->backend_available = true;
            result->success = false;
            result->error_message = exception.what();
            append_iptv_debug_log(std::string("[iptv] background exception=") + exception.what());
        } catch (...) {
            result->backend_available = true;
            result->success = false;
            result->error_message = tr("iptv_browser/unexpected_error");
            append_iptv_debug_log("[iptv] background unknown exception");
        }

        brls::sync([this, cancel_flag, result]() mutable {
            if (cancel_flag->load()) {
                append_iptv_debug_log("[iptv] sync cancelled");
                return;
            }

            try {
                append_iptv_debug_log("[iptv] apply_playlist_result begin");
                apply_playlist_result(std::move(*result));
                append_iptv_debug_log("[iptv] apply_playlist_result end");
            } catch (const std::exception& exception) {
                append_iptv_debug_log(std::string("[iptv] apply exception=") + exception.what());
                show_error_and_return_home(build_error_dialog_message(
                    tr("iptv_browser/open_failed"),
                    exception.what()));
            } catch (...) {
                append_iptv_debug_log("[iptv] apply unknown exception");
                show_error_and_return_home(build_error_dialog_message(
                    tr("iptv_browser/open_failed"),
                    tr("iptv_browser/unexpected_error")));
            }
        });
    });
}

void IptvBrowserActivity::apply_playlist_result(switchbox::core::IptvPlaylistResult result) {
    this->playlist_result = std::move(result);
    append_iptv_debug_log(
        "[iptv] playlist backend=" +
        std::string(this->playlist_result.backend_available ? "true" : "false") +
        " success=" +
        std::string(this->playlist_result.success ? "true" : "false") +
        " entries=" +
        std::to_string(this->playlist_result.entries.size()));

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

    append_iptv_debug_log("[iptv] build_group_model begin");
    build_group_model();
    append_iptv_debug_log("[iptv] build_group_model end groups=" + std::to_string(this->groups.size()));
    append_iptv_debug_log("[iptv] build_grouped_ui begin");
    build_grouped_ui();
    append_iptv_debug_log("[iptv] build_grouped_ui end");
    install_common_actions();
}

void IptvBrowserActivity::show_error_and_return_home(const std::string& message) {
    append_iptv_debug_log("[iptv] show_error_and_return_home message=" + sanitize_iptv_log_text(message));
    this->grouped_ui_ready = false;
    this->sidebar = nullptr;
    this->right_panel_root = nullptr;
    this->right_panel_title_label = nullptr;
    this->right_panel_count_label = nullptr;
    this->right_panel_empty_label = nullptr;
    this->right_recycler_frame = nullptr;
    this->visible_entry_indices.clear();

    auto* dialog = new brls::Dialog(message);
    dialog->addButton(
        brls::getStr("hints/ok"),
        [this]() {
            return_to_previous_activity();
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

    std::unordered_map<std::string, size_t> group_index_by_title;
    group_index_by_title.reserve(this->playlist_result.entries.size());

    for (size_t index = 0; index < this->playlist_result.entries.size(); ++index) {
        const auto& entry = this->playlist_result.entries[index];
        const std::string group_title =
            entry.group_title.empty() ? tr("iptv_browser/groups/ungrouped") : entry.group_title;

        const auto existing = group_index_by_title.find(group_title);
        if (existing == group_index_by_title.end()) {
            this->groups.push_back({
                .title = group_title,
                .entry_indices = {index},
                .favorites = false,
            });
            group_index_by_title.emplace(group_title, this->groups.size() - 1);
            continue;
        }

        this->groups[existing->second].entry_indices.push_back(index);
    }

    if (this->selected_group_index >= this->groups.size()) {
        this->selected_group_index = 0;
    }

    if (!this->groups.empty() && this->selected_group_index < this->groups.size()) {
        append_iptv_debug_log(
            "[iptv] default_group index=" + std::to_string(this->selected_group_index) +
            " title=" + this->groups[this->selected_group_index].title +
            " entries=" +
            std::to_string(this->groups[this->selected_group_index].entry_indices.size()));
    }
}

void IptvBrowserActivity::build_grouped_ui() {
    append_iptv_debug_log("[iptv] build_grouped_ui root begin");
    auto* root = new brls::Box(brls::Axis::ROW);

    auto* sidebar = new brls::Sidebar();
    sidebar->setWidth(280);
    sidebar->setGrow(0);
    sidebar->setScrollingIndicatorVisible(true);
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
    right_content->setGrow(1.0f);
    this->right_panel_root = right_content;

    auto theme = brls::Application::getTheme();

    auto* title_label = create_label({}, 20.0f, theme["brls/text"], true);
    title_label->setMargins(0, 0, 8, 0);
    this->right_panel_title_label = title_label;
    right_content->addView(title_label);

    auto* count_label = create_label({}, 15.0f, theme["brls/text_disabled"], true);
    count_label->setMargins(0, 0, 16, 0);
    this->right_panel_count_label = count_label;
    right_content->addView(count_label);

    auto* empty_label = create_focusable_message_label({}, 17.0f, theme["brls/text_disabled"]);
    empty_label->setFocusable(false);
    empty_label->setMargins(0, 8, 8, 0);
    empty_label->setVisibility(brls::Visibility::GONE);
    this->right_panel_empty_label = empty_label;
    right_content->addView(empty_label);

    auto* recycler = new brls::RecyclerFrame();
    recycler->setGrow(1.0f);
    recycler->setFocusScrollBehavior(brls::RecyclerFocusScrollBehavior::ENSURE_VISIBLE);
    recycler->estimatedRowHeight = brls::Application::getStyle()["brls/dropdown/listItemHeight"];
    recycler->registerCell("switchbox::iptv_entry", []() {
        return static_cast<brls::RecyclerCell*>(new IptvEntryCell());
    });
    this->right_panel_data_source = std::make_unique<IptvBrowserDataSource>(this);
    recycler->setDataSource(this->right_panel_data_source.get(), false);
    this->right_recycler_frame = recycler;
    right_content->addView(recycler);

    root->addView(right_content);

    auto* frame = new brls::AppletFrame(root);
    frame->setTitle(visible_iptv_title(this->source));
    apply_header_subtitle(frame, this->source.url, brls::Application::getTheme()["brls/text_disabled"]);
    append_iptv_debug_log("[iptv] setContentView begin");
    setContentView(frame);
    append_iptv_debug_log("[iptv] setContentView end");

    this->grouped_ui_ready = true;
    append_iptv_debug_log("[iptv] rebuild_right_panel initial begin");
    rebuild_right_panel("", 0, false);
    append_iptv_debug_log("[iptv] rebuild_right_panel initial end");
    append_iptv_debug_log("[iptv] initial focus uses favorites sidebar");
    focus_sidebar();
}

void IptvBrowserActivity::rebuild_right_panel(
    const std::string& preferred_entry_key,
    size_t fallback_index,
    bool request_focus) {
    if (!this->grouped_ui_ready ||
        this->right_panel_root == nullptr ||
        this->right_panel_title_label == nullptr ||
        this->right_panel_count_label == nullptr ||
        this->right_panel_empty_label == nullptr ||
        this->right_recycler_frame == nullptr) {
        return;
    }

    this->right_panel_root->setLastFocusedView(nullptr);
    this->visible_entry_indices = current_group_entry_indices();
    const auto& group = this->groups[this->selected_group_index];
    append_iptv_debug_log(
        "[iptv] rebuild_right_panel group=" + group.title +
        " entries=" + std::to_string(this->visible_entry_indices.size()) +
        " request_focus=" + std::string(request_focus ? "true" : "false"));

    this->right_panel_title_label->setText(group.title);
    this->right_panel_count_label->setText(
        tr("iptv_browser/group_count", std::to_string(this->visible_entry_indices.size())));

    size_t target_row = fallback_index;
    if (!preferred_entry_key.empty()) {
        for (size_t position = 0; position < this->visible_entry_indices.size(); ++position) {
            const auto& entry = this->playlist_result.entries[this->visible_entry_indices[position]];
            if (entry.favorite_key == preferred_entry_key) {
                target_row = position;
                break;
            }
        }
    }
    if (!this->visible_entry_indices.empty()) {
        target_row = std::min(target_row, this->visible_entry_indices.size() - 1);
    } else {
        target_row = 0;
    }

    if (this->visible_entry_indices.empty()) {
        const std::string empty_key =
            group.favorites ? "iptv_browser/favorites_empty" : "iptv_browser/group_empty";
        this->right_panel_empty_label->setText(tr(empty_key));
        this->right_panel_empty_label->setVisibility(brls::Visibility::VISIBLE);
        this->right_recycler_frame->setVisibility(brls::Visibility::VISIBLE);
        this->right_recycler_frame->setFocusable(false);
        this->right_recycler_frame->setLastFocusedView(nullptr);
        this->right_recycler_frame->reloadData();
    } else {
        this->right_panel_empty_label->setVisibility(brls::Visibility::GONE);
        this->right_recycler_frame->setVisibility(brls::Visibility::VISIBLE);
        this->right_recycler_frame->setFocusable(true);
        this->right_recycler_frame->setDefaultCellFocus(brls::IndexPath(0, target_row));
        this->right_recycler_frame->reloadData();
    }

    if (request_focus) {
        if (this->visible_entry_indices.empty()) {
            append_iptv_debug_log("[iptv] rebuild_right_panel fallback_sidebar_empty");
            focus_sidebar();
        } else if (auto* focus_target = this->right_recycler_frame->getDefaultFocus()) {
            append_iptv_debug_log("[iptv] rebuild_right_panel giveFocus recycler");
            brls::Application::giveFocus(focus_target);
        } else {
            append_iptv_debug_log("[iptv] rebuild_right_panel fallback_sidebar");
            focus_sidebar();
        }
    }
}

void IptvBrowserActivity::select_group(size_t group_index) {
    append_iptv_debug_log("[iptv] select_group request=" + std::to_string(group_index));
    if (!this->grouped_ui_ready || group_index >= this->groups.size()) {
        append_iptv_debug_log("[iptv] select_group ignored");
        return;
    }

    if (group_index == this->selected_group_index && this->right_panel_root != nullptr) {
        append_iptv_debug_log("[iptv] select_group no-op");
        return;
    }

    this->selected_group_index = group_index;
    if (this->right_recycler_frame != nullptr) {
        this->right_recycler_frame->setContentOffsetY(0.0f, false);
    }
    rebuild_right_panel();
    append_iptv_debug_log("[iptv] select_group applied=" + std::to_string(group_index));
}

bool IptvBrowserActivity::handle_back_action() {
    if (!this->grouped_ui_ready) {
        append_iptv_debug_log("[iptv] handle_back_action pop_loading");
        this->load_cancelled->store(true);
        return_to_previous_activity();
        return true;
    }

    append_iptv_debug_log("[iptv] handle_back_action confirm");
    show_exit_confirm_dialog();
    return true;
}

bool IptvBrowserActivity::is_focus_in_right_panel() const {
    if (this->right_panel_root == nullptr) {
        return false;
    }

    brls::View* focus = brls::Application::getCurrentFocus();
    return focus != nullptr && view_is_descendant_of(focus, this->right_panel_root);
}

int IptvBrowserActivity::focused_right_panel_row() const {
    if (!is_focus_in_right_panel()) {
        return -1;
    }

    brls::View* focus = brls::Application::getCurrentFocus();
    while (focus != nullptr) {
        if (auto* cell = dynamic_cast<brls::RecyclerCell*>(focus)) {
            return static_cast<int>(cell->getIndexPath().row);
        }
        focus = focus->getParent();
    }

    return -1;
}

bool IptvBrowserActivity::move_right_panel_focus(int delta) {
    if (delta == 0 || !this->grouped_ui_ready || this->right_recycler_frame == nullptr || !is_focus_in_right_panel()) {
        return false;
    }
    if (this->visible_entry_indices.empty()) {
        return false;
    }

    int current_row = focused_right_panel_row();
    if (current_row < 0) {
        current_row = static_cast<int>(this->right_recycler_frame->getDefaultCellFocus().row);
    }
    current_row = std::clamp(current_row, 0, static_cast<int>(this->visible_entry_indices.size()) - 1);

    const int target_row = std::clamp(
        current_row + delta,
        0,
        static_cast<int>(this->visible_entry_indices.size()) - 1);
    if (target_row == current_row) {
        return true;
    }

    this->right_recycler_frame->setDefaultCellFocus(brls::IndexPath(0, target_row));
    this->right_recycler_frame->selectRowAt(brls::IndexPath(0, target_row), false);
    if (auto* focus_target = this->right_recycler_frame->getDefaultFocus()) {
        brls::Application::giveFocus(focus_target);
    }
    return true;
}

void IptvBrowserActivity::show_exit_confirm_dialog() {
    if (this->exit_confirm_open) {
        return;
    }

    this->exit_confirm_open = true;
    auto* dialog = new brls::Dialog(tr("iptv_browser/exit_confirm"));
    dialog->setCancelable(false);
    dialog->addButton(
        tr("actions/confirm"),
        [this]() {
            this->exit_confirm_open = false;
            this->load_cancelled->store(true);
            append_iptv_debug_log("[iptv] exit_confirm accepted");
            return_to_previous_activity();
        });
    dialog->addButton(
        brls::getStr("hints/cancel"),
        [this]() {
            this->exit_confirm_open = false;
            append_iptv_debug_log("[iptv] exit_confirm cancelled");
        });
    if (auto* cancel_button = dialog->getView("brls/dialog/button2")) {
        dialog->setLastFocusedView(cancel_button);
    }
    dialog->open();
    brls::delay(1, [dialog]() {
        if (auto* cancel_button = dialog->getView("brls/dialog/button2")) {
            dialog->setLastFocusedView(cancel_button);
            brls::Application::giveFocus(cancel_button);
        }
    });
}

void IptvBrowserActivity::focus_sidebar() {
    if (!this->grouped_ui_ready || this->sidebar == nullptr || this->groups.empty()) {
        append_iptv_debug_log("[iptv] focus_sidebar skipped");
        return;
    }

    append_iptv_debug_log("[iptv] focus_sidebar getItem");
    if (auto* item = this->sidebar->getItem(static_cast<int>(this->selected_group_index))) {
        append_iptv_debug_log("[iptv] focus_sidebar giveFocus");
        brls::Application::giveFocus(item);
        return;
    }

    append_iptv_debug_log("[iptv] focus_sidebar item_missing");
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
    std::unordered_set<std::string> emitted_keys;
    std::vector<size_t> indices;
    indices.reserve(this->playlist_result.entries.size());

    for (size_t index = 0; index < this->playlist_result.entries.size(); ++index) {
        const auto& favorite_key = this->playlist_result.entries[index].favorite_key;
        if (favorite_keys.contains(favorite_key) && emitted_keys.emplace(favorite_key).second) {
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
    std::vector<std::string> normalized_keys;
    normalized_keys.reserve(favorite_keys.size());
    std::unordered_set<std::string> seen_keys;
    for (const auto& favorite_key : favorite_keys) {
        if (favorite_key.empty()) {
            continue;
        }

        if (seen_keys.emplace(favorite_key).second) {
            normalized_keys.push_back(favorite_key);
        }
    }

    this->source.favorite_keys = normalized_keys;

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
        config_it->favorite_keys = normalized_keys;
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

    const bool focus_in_right_panel = is_focus_in_right_panel();
    const bool current_group_is_favorites =
        this->selected_group_index < this->groups.size() && this->groups[this->selected_group_index].favorites;
    const float previous_content_offset =
        this->right_recycler_frame != nullptr ? this->right_recycler_frame->getContentOffsetY() : 0.0f;

    if (current_group_is_favorites) {
        const std::string preferred_key = removing ? std::string{} : entry.favorite_key;
        rebuild_right_panel(preferred_key, fallback_index, true);
        return true;
    }

    if (this->right_recycler_frame != nullptr) {
        this->right_recycler_frame->setDefaultCellFocus(brls::IndexPath(0, fallback_index));
        this->right_recycler_frame->reloadData();
        this->right_recycler_frame->setContentOffsetY(previous_content_offset, false);
        if (focus_in_right_panel) {
            if (auto* focus_target = this->right_recycler_frame->getDefaultFocus()) {
                brls::Application::giveFocus(focus_target);
                this->right_recycler_frame->setContentOffsetY(previous_content_offset, false);
            }
        }
    }
    return true;
}

void IptvBrowserActivity::return_to_previous_activity() {
    brls::Application::popActivity(
        brls::TransitionAnimation::FADE,
        []() {
            const auto activities = brls::Application::getActivitiesStack();
            if (activities.empty()) {
                return;
            }

            auto* current_activity = activities.back();
            auto* current_focus = brls::Application::getCurrentFocus();
            if (current_focus != nullptr && current_focus->getParentActivity() == current_activity) {
                return;
            }

            if (auto* fallback_focus = current_activity->getDefaultFocus()) {
                brls::Application::giveFocus(fallback_focus);
            }
        });
}

void IptvBrowserActivity::return_to_home() {
    this->load_cancelled->store(true);
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
