#include "switchbox/app/settings_activity.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <borealis/core/i18n.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_bool.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/cells/cell_input.hpp>
#include <borealis/views/dropdown.hpp>
#include <borealis/views/hint.hpp>
#include <borealis/views/image.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>
#include <borealis/views/sidebar.hpp>

#include "switchbox/app/application.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/core/language.hpp"
#include "switchbox/core/playback_history.hpp"
#include "switchbox/core/update_check.hpp"

namespace switchbox::app {

enum class SettingsSection {
    Donate,
    General,
    Update,
    Iptv,
    Smb,
};

enum class PreferredLanguagePreset {
    Chinese,
    English,
    Custom,
};

struct LanguageOption {
    std::string value;
    std::string label;
};

enum class UpdateCheckStatus {
    Idle,
    Loading,
    Ready,
    Failed,
};

struct SettingsDraftState {
    switchbox::core::AppConfig saved_config;
    switchbox::core::AppConfig draft_config;
    SettingsSection active_section = SettingsSection::General;
    bool dirty = false;
    bool ui_ready = false;
    brls::Sidebar* sidebar = nullptr;
    brls::Box* right_content_box = nullptr;
    brls::ScrollingFrame* right_scrolling_frame = nullptr;
    std::string focus_restore_id;
    UpdateCheckStatus update_check_status = UpdateCheckStatus::Idle;
    std::string latest_release_version;
    std::string latest_release_error;
    std::shared_ptr<std::atomic_bool> update_check_cancel_flag = std::make_shared<std::atomic_bool>(false);
    bool update_check_requested = false;
};

namespace {

class StaticSafeScrollingFrame : public brls::ScrollingFrame {
public:
    void setTouchHitTestEnabled(bool enabled) {
        this->touch_hit_test_enabled = enabled;
    }

    brls::View* hitTest(brls::Point point) override {
        if (!this->touch_hit_test_enabled) {
            return nullptr;
        }

        return brls::ScrollingFrame::hitTest(point);
    }

private:
    bool touch_hit_test_enabled = true;
};

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

std::string tr(const std::string& key, const std::string& arg) {
    return brls::getStr("switchbox/" + key, arg);
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

constexpr std::string_view kProjectGithubUrl = "https://github.com/WangBin-star/SwitchBOX";
constexpr std::string_view kProjectQqGroup = "1022585620";

std::string update_latest_version_display_text(const std::shared_ptr<SettingsDraftState>& state) {
    switch (state->update_check_status) {
        case UpdateCheckStatus::Ready:
            return state->latest_release_version.empty()
                ? tr("settings_page/update/latest_version_unavailable")
                : state->latest_release_version;
        case UpdateCheckStatus::Failed:
            return tr("settings_page/update/latest_version_unavailable");
        case UpdateCheckStatus::Loading:
        case UpdateCheckStatus::Idle:
        default:
            return tr("settings_page/update/latest_version_loading");
    }
}

brls::DetailCell* create_info_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detail_color) {
    constexpr size_t kDetailMaxChars = 40;
    std::string clipped_detail = detail;
    if (clipped_detail.size() > kDetailMaxChars) {
        clipped_detail = clipped_detail.substr(0, kDetailMaxChars - 3) + "...";
    }

    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(clipped_detail);
    cell->setDetailTextColor(detail_color);
    return cell;
}

brls::DetailCell* create_action_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detail_color,
    std::function<bool(brls::View*)> action,
    const std::string& view_id = "") {
    auto* cell = create_info_cell(title, detail, detail_color);
    if (!view_id.empty()) {
        cell->setId(view_id);
    }
    cell->registerClickAction(std::move(action));
    return cell;
}

brls::InputCell* create_input_cell(
    const std::string& title,
    const std::string& value,
    const std::string& placeholder,
    const std::string& hint,
    brls::Event<std::string>::Callback callback) {
    auto* cell = new brls::InputCell();
    cell->init(title, value, std::move(callback), placeholder, hint);
    return cell;
}

brls::BooleanCell* create_bool_cell(
    const std::string& title,
    bool is_on,
    std::function<void(bool)> callback) {
    auto* cell = new brls::BooleanCell();
    cell->init(title, is_on, std::move(callback));
    return cell;
}

void configure_applet_frame(brls::AppletFrame* frame, const std::string& title) {
    frame->setTitle(title);

    if (auto* hints = dynamic_cast<brls::Hints*>(frame->getView("brls/hints"))) {
        hints->setAllowAButtonTouch(true);
    }
}

bool general_settings_equal(
    const switchbox::core::GeneralSettings& lhs,
    const switchbox::core::GeneralSettings& rhs) {
    return lhs.language == rhs.language &&
           lhs.playable_extensions == rhs.playable_extensions &&
           lhs.sort_order == rhs.sort_order &&
           lhs.exit_to_home_screen == rhs.exit_to_home_screen &&
           lhs.hardware_decode == rhs.hardware_decode &&
           lhs.short_seek == rhs.short_seek &&
           lhs.long_seek == rhs.long_seek &&
           lhs.y_hold_speed_multiplier == rhs.y_hold_speed_multiplier &&
           lhs.continuous_seek_interval_ms == rhs.continuous_seek_interval_ms &&
           lhs.player_volume == rhs.player_volume &&
           lhs.use_preferred_audio_language == rhs.use_preferred_audio_language &&
           lhs.preferred_audio_language == rhs.preferred_audio_language &&
           lhs.use_preferred_subtitle_language == rhs.use_preferred_subtitle_language &&
           lhs.preferred_subtitle_language == rhs.preferred_subtitle_language &&
           lhs.demux_cache_sec == rhs.demux_cache_sec &&
           lhs.resume_start_percent == rhs.resume_start_percent &&
           lhs.resume_stop_percent == rhs.resume_stop_percent &&
           lhs.touch_enable == rhs.touch_enable &&
           lhs.touch_player_gestures == rhs.touch_player_gestures;
}

bool iptv_source_equal(
    const switchbox::core::IptvSourceSettings& lhs,
    const switchbox::core::IptvSourceSettings& rhs) {
    return lhs.key == rhs.key &&
           lhs.title == rhs.title &&
           lhs.url == rhs.url &&
           lhs.enabled == rhs.enabled &&
           lhs.use_history == rhs.use_history &&
           lhs.favorite_keys == rhs.favorite_keys;
}

bool smb_source_equal(
    const switchbox::core::SmbSourceSettings& lhs,
    const switchbox::core::SmbSourceSettings& rhs) {
    return lhs.key == rhs.key &&
           lhs.title == rhs.title &&
           lhs.host == rhs.host &&
           lhs.share == rhs.share &&
           lhs.username == rhs.username &&
           lhs.password == rhs.password &&
           lhs.enabled == rhs.enabled &&
           lhs.use_history == rhs.use_history;
}

bool iptv_sources_equal(
    const std::vector<switchbox::core::IptvSourceSettings>& lhs,
    const std::vector<switchbox::core::IptvSourceSettings>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!iptv_source_equal(lhs[index], rhs[index])) {
            return false;
        }
    }

    return true;
}

bool smb_sources_equal(
    const std::vector<switchbox::core::SmbSourceSettings>& lhs,
    const std::vector<switchbox::core::SmbSourceSettings>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!smb_source_equal(lhs[index], rhs[index])) {
            return false;
        }
    }

    return true;
}

bool app_config_equal(const switchbox::core::AppConfig& lhs, const switchbox::core::AppConfig& rhs) {
    return general_settings_equal(lhs.general, rhs.general) &&
           iptv_sources_equal(lhs.iptv_sources, rhs.iptv_sources) &&
           smb_sources_equal(lhs.smb_sources, rhs.smb_sources);
}

std::string raw_system_locale() {
    if (auto* platform = brls::Application::getPlatform()) {
        return platform->getLocale();
    }

    return brls::LOCALE_DEFAULT;
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }

    return true;
}

std::string normalize_language_tag_for_display(std::string value) {
    value = trim(std::move(value));

    if (value.empty()) {
        return {};
    }

    std::replace(value.begin(), value.end(), '_', '-');

    if (equals_ignore_case(value, "auto")) {
        return "auto";
    }

    if (equals_ignore_case(value, "zh-cn") || equals_ignore_case(value, "zh-sg") ||
        equals_ignore_case(value, "zh-hans")) {
        return "zh-Hans";
    }

    if (equals_ignore_case(value, "zh-tw") || equals_ignore_case(value, "zh-hk") ||
        equals_ignore_case(value, "zh-mo") || equals_ignore_case(value, "zh-hant")) {
        return "zh-Hant";
    }

    if (equals_ignore_case(value, "en") || equals_ignore_case(value, "en-us")) {
        return "en-US";
    }

    if (equals_ignore_case(value, "ja")) {
        return "ja";
    }

    if (equals_ignore_case(value, "ko")) {
        return "ko";
    }

    if (equals_ignore_case(value, "fr")) {
        return "fr";
    }

    if (equals_ignore_case(value, "ru")) {
        return "ru";
    }

    if (value.size() == 2) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    if (value.size() >= 5 && value[2] == '-') {
        std::string normalized;
        normalized.reserve(value.size());
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[0]))));
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[1]))));
        normalized.push_back('-');

        for (size_t index = 3; index < value.size(); ++index) {
            const unsigned char character = static_cast<unsigned char>(value[index]);
            normalized.push_back(static_cast<char>(index == 3 ? std::toupper(character) : std::tolower(character)));
        }

        return normalized;
    }

    return value;
}

std::string language_autonym(const std::string& locale) {
    if (locale == "en-US") {
        return "English (US)";
    }

    if (locale == "zh-Hans") {
        return "简体中文";
    }

    if (locale == "zh-Hant") {
        return "繁體中文";
    }

    if (locale == "ja") {
        return "日本語";
    }

    if (locale == "ko") {
        return "한국어";
    }

    if (locale == "fr") {
        return "Français";
    }

    if (locale == "ru") {
        return "Русский";
    }

    return locale;
}

std::vector<LanguageOption> build_language_options(const switchbox::core::LanguageState& language_state) {
    std::string system_language = normalize_language_tag_for_display(raw_system_locale());
    if (system_language.empty() || system_language == "auto") {
        system_language = language_state.active_language;
    }

    std::vector<LanguageOption> options;
    options.push_back({
        .value = "auto",
        .label = tr(
            "settings_page/language/options/auto",
            language_autonym(system_language)),
    });

    for (const auto& locale : language_state.available_languages) {
        options.push_back({
            .value = locale,
            .label = language_autonym(locale),
        });
    }

    return options;
}

int find_language_selection(
    const std::vector<LanguageOption>& options,
    const std::string& selected_value) {
    const auto selected = std::find_if(
        options.begin(),
        options.end(),
        [&selected_value](const LanguageOption& option) {
            return option.value == selected_value;
        });

    if (selected == options.end()) {
        return 0;
    }

    return static_cast<int>(std::distance(options.begin(), selected));
}

std::string selected_language_display_name(const std::shared_ptr<SettingsDraftState>& state) {
    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto language_state =
        switchbox::core::resolve_language_state(paths, state->draft_config, raw_system_locale());
    const auto options = build_language_options(language_state);
    const std::string selected_value = language_state.using_auto ? "auto" : language_state.configured_language;
    const auto selected = std::find_if(
        options.begin(),
        options.end(),
        [&selected_value](const LanguageOption& option) {
            return option.value == selected_value;
        });
    if (selected != options.end()) {
        return selected->label;
    }

    return language_autonym(language_state.active_language);
}

std::string visible_entry_title(const std::string& title) {
    if (!title.empty()) {
        return title;
    }

    return tr("settings_page/common/untitled_entry");
}

[[maybe_unused]] std::string status_prefixed_title(bool enabled, const std::string& title) {
    return std::string(enabled ? "● " : "○ ") + visible_entry_title(title);
}

std::string summarize_iptv_source(const switchbox::core::IptvSourceSettings& source) {
    if (!source.url.empty()) {
        return source.url;
    }

    return tr("settings_page/common/not_set");
}

std::string summarize_smb_source(const switchbox::core::SmbSourceSettings& source) {
    if (!source.host.empty() && !source.share.empty()) {
        return source.host + "/" + source.share;
    }

    if (!source.host.empty()) {
        return source.host;
    }

    if (!source.share.empty()) {
        return source.share;
    }

    return tr("settings_page/common/not_set");
}

std::string summarize_detail_text(const std::string& value, size_t max_length = 56) {
    if (value.size() <= max_length) {
        return value;
    }

    if (max_length <= 3) {
        return value.substr(0, max_length);
    }

    return value.substr(0, max_length - 3) + "...";
}

std::string bool_display_text(bool value) {
    return value ? tr("settings_page/common/enabled") : tr("settings_page/common/disabled");
}

std::string sort_order_display_name(const std::string& sort_order) {
    if (sort_order == "name_desc") {
        return tr("settings_page/general/sort_order/options/name_desc");
    }
    if (sort_order == "date_asc") {
        return tr("settings_page/general/sort_order/options/date_asc");
    }
    if (sort_order == "date_desc") {
        return tr("settings_page/general/sort_order/options/date_desc");
    }
    if (sort_order == "size_asc") {
        return tr("settings_page/general/sort_order/options/size_asc");
    }
    if (sort_order == "size_desc") {
        return tr("settings_page/general/sort_order/options/size_desc");
    }
    return tr("settings_page/general/sort_order/options/name_asc");
}

PreferredLanguagePreset detect_preferred_language_preset(const std::string& value) {
    if (value == "zh") {
        return PreferredLanguagePreset::Chinese;
    }
    if (value == "en") {
        return PreferredLanguagePreset::English;
    }
    return PreferredLanguagePreset::Custom;
}

std::string preferred_language_display_text(const std::string& value) {
    switch (detect_preferred_language_preset(value)) {
        case PreferredLanguagePreset::Chinese:
            return tr("settings_page/general/preferred_language_options/chinese");
        case PreferredLanguagePreset::English:
            return tr("settings_page/general/preferred_language_options/english");
        case PreferredLanguagePreset::Custom:
        default:
            return value.empty() ? tr("settings_page/general/preferred_language_options/custom")
                                 : value;
    }
}

std::string preferred_language_custom_seed(const std::string& value) {
    switch (detect_preferred_language_preset(value)) {
        case PreferredLanguagePreset::Chinese:
        case PreferredLanguagePreset::English:
            return {};
        case PreferredLanguagePreset::Custom:
        default:
            return value;
    }
}

std::string format_float_value(float value, int precision = 1) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(precision);
    stream << value;
    return stream.str();
}

void request_focus_restore(const std::shared_ptr<SettingsDraftState>& state, const std::string& view_id) {
    state->focus_restore_id = view_id;
}

std::string make_iptv_source_view_id(const std::string& key) {
    return "settings/iptv/source/" + key;
}

std::string make_smb_source_view_id(const std::string& key) {
    return "settings/smb/source/" + key;
}

std::string status_icon_title(bool enabled, const std::string& title) {
    return std::string(enabled ? "● " : "○ ") + visible_entry_title(title);
}

void sync_dirty_state(const std::shared_ptr<SettingsDraftState>& state) {
    state->dirty = !app_config_equal(state->draft_config, state->saved_config);
    if (state->ui_ready) {
        brls::Application::getGlobalHintsUpdateEvent()->fire();
    }
}

switchbox::core::IptvSourceSettings* find_iptv_source(
    const std::shared_ptr<SettingsDraftState>& state,
    const std::string& key) {
    const auto it = std::find_if(
        state->draft_config.iptv_sources.begin(),
        state->draft_config.iptv_sources.end(),
        [&key](const switchbox::core::IptvSourceSettings& source) {
            return source.key == key;
        });

    if (it == state->draft_config.iptv_sources.end()) {
        return nullptr;
    }

    return &(*it);
}

switchbox::core::SmbSourceSettings* find_smb_source(
    const std::shared_ptr<SettingsDraftState>& state,
    const std::string& key) {
    const auto it = std::find_if(
        state->draft_config.smb_sources.begin(),
        state->draft_config.smb_sources.end(),
        [&key](const switchbox::core::SmbSourceSettings& source) {
            return source.key == key;
        });

    if (it == state->draft_config.smb_sources.end()) {
        return nullptr;
    }

    return &(*it);
}

std::string make_unique_iptv_key(const std::shared_ptr<SettingsDraftState>& state) {
    int index = 1;
    while (true) {
        const std::string key = "source-" + std::to_string(index);
        if (find_iptv_source(state, key) == nullptr) {
            return key;
        }
        ++index;
    }
}

std::string make_unique_smb_key(const std::shared_ptr<SettingsDraftState>& state) {
    int index = 1;
    while (true) {
        const std::string key = "source-" + std::to_string(index);
        if (find_smb_source(state, key) == nullptr) {
            return key;
        }
        ++index;
    }
}

void rebuild_right_panel(const std::shared_ptr<SettingsDraftState>& state);

void upsert_iptv_source(
    const std::shared_ptr<SettingsDraftState>& state,
    const switchbox::core::IptvSourceSettings& source) {
    if (auto* existing = find_iptv_source(state, source.key)) {
        *existing = source;
    } else {
        state->draft_config.iptv_sources.push_back(source);
    }

    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void upsert_smb_source(
    const std::shared_ptr<SettingsDraftState>& state,
    const switchbox::core::SmbSourceSettings& source) {
    if (auto* existing = find_smb_source(state, source.key)) {
        *existing = source;
    } else {
        state->draft_config.smb_sources.push_back(source);
    }

    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void remove_iptv_source(
    const std::shared_ptr<SettingsDraftState>& state,
    const std::string& key) {
    auto& sources = state->draft_config.iptv_sources;
    sources.erase(
        std::remove_if(
            sources.begin(),
            sources.end(),
            [&key](const switchbox::core::IptvSourceSettings& source) {
                return source.key == key;
            }),
        sources.end());

    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void remove_smb_source(
    const std::shared_ptr<SettingsDraftState>& state,
    const std::string& key) {
    auto& sources = state->draft_config.smb_sources;
    sources.erase(
        std::remove_if(
            sources.begin(),
            sources.end(),
            [&key](const switchbox::core::SmbSourceSettings& source) {
                return source.key == key;
            }),
        sources.end());

    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void confirm_delete_entry(
    const std::string& message,
    brls::VoidEvent::Callback on_confirm) {
    auto* dialog = new brls::Dialog(message);
    dialog->addButton(brls::getStr("hints/cancel"), []() {});
    dialog->addButton(brls::getStr("hints/ok"), std::move(on_confirm));
    dialog->open();
}

brls::AppletFrame* create_editor_frame(brls::Box* content, const std::string& title) {
    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(content);

    auto* frame = new brls::AppletFrame(scrolling_frame);
    configure_applet_frame(frame, title);
    return frame;
}

brls::View* create_iptv_editor_content(
    const std::shared_ptr<SettingsDraftState>& state,
    switchbox::core::IptvSourceSettings source) {
    auto editor_state = std::make_shared<switchbox::core::IptvSourceSettings>(std::move(source));

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(24, 40, 24, 40);

    content->addView(create_input_cell(
        tr("settings_page/iptv/fields/title"),
        editor_state->title,
        tr("settings_page/iptv/placeholders/title"),
        tr("settings_page/iptv/hints/title"),
        [editor_state](const std::string& value) {
            editor_state->title = value;
        }));

    content->addView(create_bool_cell(
        tr("settings_page/iptv/fields/enabled"),
        editor_state->enabled,
        [editor_state](bool enabled) {
            editor_state->enabled = enabled;
        }));

    content->addView(create_bool_cell(
        tr("settings_page/iptv/fields/use_history"),
        editor_state->use_history,
        [editor_state](bool use_history) {
            editor_state->use_history = use_history;
        }));

    content->addView(create_input_cell(
        tr("settings_page/iptv/fields/url"),
        editor_state->url,
        tr("settings_page/iptv/placeholders/url"),
        tr("settings_page/iptv/hints/url"),
        [editor_state](const std::string& value) {
            editor_state->url = value;
        }));

    auto* frame = create_editor_frame(content, tr("settings_page/iptv/editor_title"));
    frame->registerAction(
        tr("actions/save"),
        brls::BUTTON_START,
        [state, editor_state](brls::View*) {
            brls::Application::popActivity(
                brls::TransitionAnimation::FADE,
                [state, editor_state]() {
                    upsert_iptv_source(state, *editor_state);
                });
            return true;
        },
        false,
        false,
        brls::SOUND_CLICK);
    return frame;
}

brls::View* create_smb_editor_content(
    const std::shared_ptr<SettingsDraftState>& state,
    switchbox::core::SmbSourceSettings source) {
    auto editor_state = std::make_shared<switchbox::core::SmbSourceSettings>(std::move(source));

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(24, 40, 24, 40);

    content->addView(create_input_cell(
        tr("settings_page/smb/fields/title"),
        editor_state->title,
        tr("settings_page/smb/placeholders/title"),
        tr("settings_page/smb/hints/title"),
        [editor_state](const std::string& value) {
            editor_state->title = value;
        }));

    content->addView(create_bool_cell(
        tr("settings_page/smb/fields/enabled"),
        editor_state->enabled,
        [editor_state](bool enabled) {
            editor_state->enabled = enabled;
        }));

    content->addView(create_bool_cell(
        tr("settings_page/smb/fields/use_history"),
        editor_state->use_history,
        [editor_state](bool use_history) {
            editor_state->use_history = use_history;
        }));

    content->addView(create_input_cell(
        tr("settings_page/smb/fields/host"),
        editor_state->host,
        tr("settings_page/smb/placeholders/host"),
        tr("settings_page/smb/hints/host"),
        [editor_state](const std::string& value) {
            editor_state->host = value;
        }));

    content->addView(create_input_cell(
        tr("settings_page/smb/fields/share"),
        editor_state->share,
        tr("settings_page/smb/placeholders/share"),
        tr("settings_page/smb/hints/share"),
        [editor_state](const std::string& value) {
            editor_state->share = value;
        }));

    content->addView(create_input_cell(
        tr("settings_page/smb/fields/username"),
        editor_state->username,
        tr("settings_page/smb/placeholders/username"),
        tr("settings_page/smb/hints/username"),
        [editor_state](const std::string& value) {
            editor_state->username = value;
        }));

    content->addView(create_input_cell(
        tr("settings_page/smb/fields/password"),
        editor_state->password,
        tr("settings_page/smb/placeholders/password"),
        tr("settings_page/smb/hints/password"),
        [editor_state](const std::string& value) {
            editor_state->password = value;
        }));

    auto* frame = create_editor_frame(content, tr("settings_page/smb/editor_title"));
    frame->registerAction(
        tr("actions/save"),
        brls::BUTTON_START,
        [state, editor_state](brls::View*) {
            brls::Application::popActivity(
                brls::TransitionAnimation::FADE,
                [state, editor_state]() {
                    upsert_smb_source(state, *editor_state);
                });
            return true;
        },
        false,
        false,
        brls::SOUND_CLICK);
    return frame;
}

void open_iptv_editor(
    const std::shared_ptr<SettingsDraftState>& state,
    switchbox::core::IptvSourceSettings source) {
    brls::Application::pushActivity(new brls::Activity(create_iptv_editor_content(state, std::move(source))));
}

void open_smb_editor(
    const std::shared_ptr<SettingsDraftState>& state,
    switchbox::core::SmbSourceSettings source) {
    brls::Application::pushActivity(new brls::Activity(create_smb_editor_content(state, std::move(source))));
}

void select_section(const std::shared_ptr<SettingsDraftState>& state, SettingsSection section) {
    if (state == nullptr) {
        return;
    }

    if (state->active_section == section) {
        return;
    }

    state->active_section = section;
    rebuild_right_panel(state);
    if (state->right_scrolling_frame != nullptr) {
        state->right_scrolling_frame->setContentOffsetY(0.0f, false);
    }
}

bool apply_draft_changes(const std::shared_ptr<SettingsDraftState>& state) {
    sync_dirty_state(state);

    if (!state->dirty) {
        brls::Application::notify(tr("settings_page/apply/detail_clean"));
        return true;
    }

    auto& mutable_config = switchbox::core::AppConfigStore::mutable_config();
    const switchbox::core::AppConfig previous_config = mutable_config;
    const bool language_changed =
        state->saved_config.general.language != state->draft_config.general.language;

    mutable_config = state->draft_config;

    if (!switchbox::core::AppConfigStore::save()) {
        mutable_config = previous_config;
        brls::Application::notify(tr("settings_page/save_failed"));
        return true;
    }

    (void)switchbox::core::remove_playback_history_missing_sources(
        switchbox::core::AppConfigStore::paths(),
        mutable_config);

    switchbox::app::Application::apply_runtime_preferences();

    if (language_changed) {
        state->saved_config = state->draft_config;
        brls::delay(1, []() {
            switchbox::app::Application::apply_language_and_reload_ui(false);
        });
        return true;
    }

    state->saved_config = state->draft_config;
    brls::delay(1, []() {
        switchbox::app::Application::reload_root_ui(false);
    });
    return true;
}

bool exit_settings_with_confirm_if_needed(const std::shared_ptr<SettingsDraftState>& state) {
    sync_dirty_state(state);
    if (!state->dirty) {
        brls::Application::popActivity(brls::TransitionAnimation::FADE);
        return true;
    }

    auto* dialog = new brls::Dialog(tr("settings_page/exit_confirm"));
    dialog->addButton(
        brls::getStr("hints/cancel"),
        []() {});
    dialog->addButton(
        tr("actions/confirm"),
        []() {
            brls::Application::popActivity(brls::TransitionAnimation::FADE);
        });
    if (auto* cancel_button = dialog->getView("brls/dialog/button1")) {
        dialog->setLastFocusedView(cancel_button);
    }
    dialog->open();
    brls::delay(1, [dialog]() {
        if (auto* cancel_button = dialog->getView("brls/dialog/button1")) {
            dialog->setLastFocusedView(cancel_button);
            brls::Application::giveFocus(cancel_button);
        }
    });
    return true;
}

void open_language_dropdown(const std::shared_ptr<SettingsDraftState>& state) {
    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto language_state =
        switchbox::core::resolve_language_state(paths, state->draft_config, raw_system_locale());
    const std::vector<LanguageOption> options = build_language_options(language_state);
    std::vector<std::string> option_labels;
    option_labels.reserve(options.size());
    for (const auto& option : options) {
        option_labels.push_back(option.label);
    }

    const int selected_index = find_language_selection(
        options,
        language_state.using_auto ? "auto" : language_state.configured_language);
    auto* dropdown = new brls::Dropdown(
        tr("settings_page/language/title"),
        option_labels,
        [](int) {},
        selected_index,
        [state, options](int selection) {
            if (selection < 0 || selection >= static_cast<int>(options.size())) {
                return;
            }

            state->draft_config.general.language = options[selection].value;
            sync_dirty_state(state);
            rebuild_right_panel(state);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void open_playable_extensions_editor(const std::shared_ptr<SettingsDraftState>& state) {
    auto& value = state->draft_config.general.playable_extensions;
    brls::Application::getImeManager()->openForText(
        [state](std::string text) {
            state->draft_config.general.playable_extensions = std::move(text);
            sync_dirty_state(state);
            rebuild_right_panel(state);
        },
        tr("settings_page/general/playable_extensions/title"),
        tr("settings_page/general/playable_extensions/hint"),
        512,
        value,
        0);
}

void toggle_hardware_decode(const std::shared_ptr<SettingsDraftState>& state) {
    state->draft_config.general.hardware_decode = !state->draft_config.general.hardware_decode;
    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void toggle_touch_enable(const std::shared_ptr<SettingsDraftState>& state) {
    state->draft_config.general.touch_enable = !state->draft_config.general.touch_enable;
    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void toggle_exit_to_home_screen(const std::shared_ptr<SettingsDraftState>& state) {
    state->draft_config.general.exit_to_home_screen =
        !state->draft_config.general.exit_to_home_screen;
    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void toggle_touch_player_gestures(const std::shared_ptr<SettingsDraftState>& state) {
    state->draft_config.general.touch_player_gestures =
        !state->draft_config.general.touch_player_gestures;
    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void toggle_preferred_audio_language(const std::shared_ptr<SettingsDraftState>& state) {
    state->draft_config.general.use_preferred_audio_language =
        !state->draft_config.general.use_preferred_audio_language;
    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void toggle_preferred_subtitle_language(const std::shared_ptr<SettingsDraftState>& state) {
    state->draft_config.general.use_preferred_subtitle_language =
        !state->draft_config.general.use_preferred_subtitle_language;
    sync_dirty_state(state);
    rebuild_right_panel(state);
}

void open_sort_order_dropdown(const std::shared_ptr<SettingsDraftState>& state) {
    const std::vector<std::string> values = {
        "name_asc",
        "name_desc",
        "date_asc",
        "date_desc",
        "size_asc",
        "size_desc",
    };
    std::vector<std::string> labels;
    labels.reserve(values.size());
    int selected_index = 0;

    for (size_t index = 0; index < values.size(); ++index) {
        labels.push_back(sort_order_display_name(values[index]));
        if (values[index] == state->draft_config.general.sort_order) {
            selected_index = static_cast<int>(index);
        }
    }

    auto* dropdown = new brls::Dropdown(
        tr("settings_page/general/sort_order/title"),
        labels,
        [](int) {},
        selected_index,
        [state, values](int selection) {
            if (selection < 0 || selection >= static_cast<int>(values.size())) {
                return;
            }

            state->draft_config.general.sort_order = values[selection];
            sync_dirty_state(state);
            rebuild_right_panel(state);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void open_short_seek_editor(const std::shared_ptr<SettingsDraftState>& state) {
    brls::Application::getImeManager()->openForText(
        [state](std::string text) {
            try {
                const int value = std::stoi(text);
                if (value <= 0) {
                    throw std::runtime_error("invalid");
                }

                state->draft_config.general.short_seek = value;
                sync_dirty_state(state);
                rebuild_right_panel(state);
            } catch (...) {
                brls::Application::notify(tr("settings_page/general/validation/positive_integer"));
            }
        },
        tr("settings_page/general/short_seek/title"),
        tr("settings_page/general/short_seek/hint"),
        16,
        std::to_string(state->draft_config.general.short_seek),
        0);
}

void open_long_seek_editor(const std::shared_ptr<SettingsDraftState>& state) {
    brls::Application::getImeManager()->openForText(
        [state](std::string text) {
            try {
                const int value = std::stoi(text);
                if (value <= 0) {
                    throw std::runtime_error("invalid");
                }

                state->draft_config.general.long_seek = value;
                sync_dirty_state(state);
                rebuild_right_panel(state);
            } catch (...) {
                brls::Application::notify(tr("settings_page/general/validation/positive_integer"));
            }
        },
        tr("settings_page/general/long_seek/title"),
        tr("settings_page/general/long_seek/hint"),
        16,
        std::to_string(state->draft_config.general.long_seek),
        0);
}

void open_y_hold_speed_multiplier_editor(const std::shared_ptr<SettingsDraftState>& state) {
    brls::Application::getImeManager()->openForText(
        [state](std::string text) {
            try {
                const float value = std::stof(text);
                if (value <= 0.0f) {
                    throw std::runtime_error("invalid");
                }

                state->draft_config.general.y_hold_speed_multiplier = value;
                sync_dirty_state(state);
                rebuild_right_panel(state);
            } catch (...) {
                brls::Application::notify(tr("settings_page/general/validation/positive_float"));
            }
        },
        tr("settings_page/general/y_hold_speed_multiplier/title"),
        tr("settings_page/general/y_hold_speed_multiplier/hint"),
        16,
        format_float_value(state->draft_config.general.y_hold_speed_multiplier, 1),
        0);
}

void open_continuous_seek_interval_editor(const std::shared_ptr<SettingsDraftState>& state) {
    brls::Application::getImeManager()->openForText(
        [state](std::string text) {
            try {
                const int value = std::stoi(text);
                if (value < 10) {
                    throw std::runtime_error("invalid");
                }

                state->draft_config.general.continuous_seek_interval_ms = value;
                sync_dirty_state(state);
                rebuild_right_panel(state);
            } catch (...) {
                brls::Application::notify(tr("settings_page/general/validation/positive_integer"));
            }
        },
        tr("settings_page/general/continuous_seek_interval_ms/title"),
        tr("settings_page/general/continuous_seek_interval_ms/hint"),
        16,
        std::to_string(state->draft_config.general.continuous_seek_interval_ms),
        0);
}

void open_preferred_audio_language_picker(const std::shared_ptr<SettingsDraftState>& state) {
    const std::vector<std::string> labels = {
        tr("settings_page/general/preferred_language_options/chinese"),
        tr("settings_page/general/preferred_language_options/english"),
        tr("settings_page/general/preferred_language_options/custom"),
    };

    int selected_index = 2;
    switch (detect_preferred_language_preset(state->draft_config.general.preferred_audio_language)) {
        case PreferredLanguagePreset::Chinese:
            selected_index = 0;
            break;
        case PreferredLanguagePreset::English:
            selected_index = 1;
            break;
        case PreferredLanguagePreset::Custom:
        default:
            selected_index = 2;
            break;
    }

    auto* dropdown = new brls::Dropdown(
        tr("settings_page/general/preferred_audio_language/title"),
        labels,
        [](int) {},
        selected_index,
        [state](int selection) {
            if (selection == 0) {
                state->draft_config.general.preferred_audio_language = "zh";
                sync_dirty_state(state);
                rebuild_right_panel(state);
                return;
            }

            if (selection == 1) {
                state->draft_config.general.preferred_audio_language = "en";
                sync_dirty_state(state);
                rebuild_right_panel(state);
                return;
            }

            brls::Application::getImeManager()->openForText(
                [state](std::string text) {
                    state->draft_config.general.preferred_audio_language = std::move(text);
                    sync_dirty_state(state);
                    rebuild_right_panel(state);
                },
                tr("settings_page/general/preferred_audio_language/title"),
                tr("settings_page/general/preferred_audio_language/hint"),
                32,
                preferred_language_custom_seed(state->draft_config.general.preferred_audio_language),
                0);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void open_preferred_subtitle_language_picker(const std::shared_ptr<SettingsDraftState>& state) {
    const std::vector<std::string> labels = {
        tr("settings_page/general/preferred_language_options/chinese"),
        tr("settings_page/general/preferred_language_options/english"),
        tr("settings_page/general/preferred_language_options/custom"),
    };

    int selected_index = 2;
    switch (detect_preferred_language_preset(state->draft_config.general.preferred_subtitle_language)) {
        case PreferredLanguagePreset::Chinese:
            selected_index = 0;
            break;
        case PreferredLanguagePreset::English:
            selected_index = 1;
            break;
        case PreferredLanguagePreset::Custom:
        default:
            selected_index = 2;
            break;
    }

    auto* dropdown = new brls::Dropdown(
        tr("settings_page/general/preferred_subtitle_language/title"),
        labels,
        [](int) {},
        selected_index,
        [state](int selection) {
            if (selection == 0) {
                state->draft_config.general.preferred_subtitle_language = "zh";
                sync_dirty_state(state);
                rebuild_right_panel(state);
                return;
            }

            if (selection == 1) {
                state->draft_config.general.preferred_subtitle_language = "en";
                sync_dirty_state(state);
                rebuild_right_panel(state);
                return;
            }

            brls::Application::getImeManager()->openForText(
                [state](std::string text) {
                    state->draft_config.general.preferred_subtitle_language = std::move(text);
                    sync_dirty_state(state);
                    rebuild_right_panel(state);
                },
                tr("settings_page/general/preferred_subtitle_language/title"),
                tr("settings_page/general/preferred_subtitle_language/hint"),
                32,
                preferred_language_custom_seed(state->draft_config.general.preferred_subtitle_language),
                0);
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

void start_update_check(const std::shared_ptr<SettingsDraftState>& state) {
    if (state == nullptr || state->update_check_cancel_flag == nullptr) {
        return;
    }

    state->update_check_cancel_flag->store(false);
    state->update_check_status = UpdateCheckStatus::Loading;
    state->latest_release_version.clear();
    state->latest_release_error.clear();

    auto cancel_flag = state->update_check_cancel_flag;
    brls::async([state, cancel_flag]() {
        switchbox::core::UpdateCheckResult result;
        try {
            result = switchbox::core::fetch_latest_release_version(cancel_flag);
        } catch (const std::exception& exception) {
            result.error_message = std::string("Update check failed: ") + trim(exception.what());
        } catch (...) {
            result.error_message = "Update check failed: unknown exception.";
        }

        brls::sync([state, cancel_flag, result]() {
            if (state == nullptr || state->update_check_cancel_flag != cancel_flag || cancel_flag->load()) {
                return;
            }

            state->latest_release_version = result.latest_version;
            state->latest_release_error = result.error_message;
            state->update_check_status = result.success ? UpdateCheckStatus::Ready : UpdateCheckStatus::Failed;

            if (!state->ui_ready || state->right_content_box == nullptr) {
                return;
            }

            if (state->active_section == SettingsSection::Update) {
                rebuild_right_panel(state);
            }
        });
    });
}

void schedule_update_check(const std::shared_ptr<SettingsDraftState>& state) {
    if (state == nullptr || state->update_check_requested) {
        return;
    }

    state->update_check_requested = true;
    state->update_check_status = UpdateCheckStatus::Loading;
    state->latest_release_version.clear();
    state->latest_release_error.clear();

    brls::delay(100, [state]() {
        if (state == nullptr || state->update_check_cancel_flag == nullptr || !state->ui_ready) {
            return;
        }

        start_update_check(state);
    });
}

void rebuild_update_panel(const std::shared_ptr<SettingsDraftState>& state) {
    constexpr float kQrImageWidth = 320.0f;
    constexpr float kQrImageAspect = 1430.0f / 1284.0f;
    constexpr float kSectionTitleSize = 20.0f;
    constexpr float kBodyTextSize = 18.0f;
    constexpr float kValueTextSize = 20.0f;
    constexpr float kSectionTopMargin = 26.0f;
    constexpr float kTitleToContentGap = 10.0f;

    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();
    container->setJustifyContent(brls::JustifyContent::CENTER);

    auto* layout = new brls::Box(brls::Axis::ROW);
    layout->setAlignItems(brls::AlignItems::CENTER);
    layout->setHeightPercentage(100.0f);

    auto* left_box = new brls::Box(brls::Axis::COLUMN);
    left_box->setGrow(1.0f);
    left_box->setMargins(0, 0, 0, 36);
    left_box->setJustifyContent(brls::JustifyContent::CENTER);

    auto* github_title = create_label(
        tr("settings_page/update/github_title"),
        kSectionTitleSize,
        theme["brls/highlight/color2"],
        true);
    github_title->setMargins(6, 6, kTitleToContentGap, 0);
    left_box->addView(github_title);

    auto* github_value = create_label(
        std::string(kProjectGithubUrl),
        kValueTextSize,
        theme["brls/list/listItem_value_color"],
        false);
    github_value->setMargins(6, 0, 12, 0);
    github_value->setMaxWidth(560.0f);
    github_value->setLineHeight(1.12f);
    left_box->addView(github_value);

    auto* current_title = create_label(
        tr("settings_page/update/current_version_title"),
        kSectionTitleSize,
        theme["brls/highlight/color2"],
        true);
    current_title->setMargins(6, kSectionTopMargin, kTitleToContentGap, 0);
    left_box->addView(current_title);

    auto* current_value = create_label(
        switchbox::core::BuildInfo::version_string(),
        kValueTextSize,
        theme["brls/list/listItem_value_color"],
        true);
    current_value->setMargins(6, 0, 12, 0);
    current_value->setLineHeight(1.08f);
    left_box->addView(current_value);

    auto* latest_title = create_label(
        tr("settings_page/update/latest_version_title"),
        kSectionTitleSize,
        theme["brls/highlight/color2"],
        true);
    latest_title->setMargins(6, kSectionTopMargin, kTitleToContentGap, 0);
    left_box->addView(latest_title);

    auto* latest_value = create_label(
        update_latest_version_display_text(state),
        kValueTextSize,
        theme["brls/list/listItem_value_color"],
        false);
    latest_value->setMargins(6, 0, 12, 0);
    latest_value->setMaxWidth(560.0f);
    latest_value->setLineHeight(1.12f);
    left_box->addView(latest_value);

    if (state->update_check_status == UpdateCheckStatus::Failed && !state->latest_release_error.empty()) {
        auto* latest_error = create_label(
            tr("settings_page/update/latest_version_failed", state->latest_release_error),
            15.0f,
            theme["brls/text_disabled"],
            false);
        latest_error->setMargins(6, 8, 12, 0);
        latest_error->setMaxWidth(560.0f);
        latest_error->setLineHeight(1.25f);
        left_box->addView(latest_error);
    }

    auto* community_title = create_label(
        tr("settings_page/update/community_title"),
        kSectionTitleSize,
        theme["brls/highlight/color2"],
        true);
    community_title->setMargins(6, kSectionTopMargin, kTitleToContentGap, 0);
    left_box->addView(community_title);

    auto* community_github = create_label(
        tr("settings_page/update/community_text_github"),
        kBodyTextSize,
        theme["brls/text"],
        false);
    community_github->setMargins(6, 0, 12, 0);
    community_github->setMaxWidth(560.0f);
    community_github->setLineHeight(1.24f);
    left_box->addView(community_github);

    auto* community_group = create_label(
        tr("settings_page/update/community_text_group", std::string(kProjectQqGroup)),
        kBodyTextSize,
        theme["brls/text"],
        false);
    community_group->setMargins(6, 8, 12, 0);
    community_group->setMaxWidth(560.0f);
    community_group->setLineHeight(1.24f);
    left_box->addView(community_group);

    layout->addView(left_box);

    auto* right_box = new brls::Box(brls::Axis::COLUMN);
    right_box->setAlignItems(brls::AlignItems::CENTER);
    right_box->setJustifyContent(brls::JustifyContent::CENTER);
    right_box->setWidth(360.0f);

    auto* image_hint = create_label(
        tr("settings_page/update/image_hint"),
        kSectionTitleSize,
        theme["brls/highlight/color2"],
        true);
    image_hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    image_hint->setMargins(0, 6, kTitleToContentGap, 0);
    right_box->addView(image_hint);

    auto* image = new brls::Image();
    image->setImageFromRes("img/qq_group_qr.jpg");
    image->setScalingType(brls::ImageScalingType::FIT);
    image->setDimensions(kQrImageWidth, kQrImageWidth * kQrImageAspect);
    right_box->addView(image);

    layout->addView(right_box);
    container->addView(layout);
}

void rebuild_donate_panel(const std::shared_ptr<SettingsDraftState>& state) {
    constexpr float kDonateImageWidth = 720.0f;
    constexpr float kDonateImageAspect = 448.0f / 938.0f;

    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();
    container->setJustifyContent(brls::JustifyContent::FLEX_START);

    auto* summary = create_label(
        tr("settings_page/donate/summary"),
        17.0f,
        theme["brls/header/subtitle"],
        false);
    summary->setMargins(6, 6, 16, 0);
    summary->setMaxWidth(760.0f);
    summary->setLineHeight(1.26f);
    container->addView(summary);

    auto* paypal_title = create_label(
        tr("settings_page/donate/paypal_title"),
        20.0f,
        theme["brls/highlight/color2"],
        true);
    paypal_title->setMargins(8, 18, 4, 0);
    container->addView(paypal_title);

    auto* paypal_value = create_label(
        "star_ujn@qq.com",
        22.0f,
        theme["brls/list/listItem_value_color"],
        true);
    paypal_value->setMargins(8, 0, 12, 0);
    paypal_value->setLineHeight(1.08f);
    container->addView(paypal_value);

    auto* image_hint = create_label(
        tr("settings_page/donate/image_hint"),
        16.0f,
        theme["brls/text_disabled"],
        true);
    image_hint->setMargins(8, 16, 12, 0);
    container->addView(image_hint);

    auto* image_box = new brls::Box(brls::Axis::COLUMN);
    image_box->setAlignItems(brls::AlignItems::CENTER);
    image_box->setMargins(8, 0, 20, 0);

    auto* image = new brls::Image();
    image->setImageFromRes("img/donation_qr.png");
    image->setScalingType(brls::ImageScalingType::FIT);
    image->setDimensions(kDonateImageWidth, kDonateImageWidth * kDonateImageAspect);
    image_box->addView(image);

    container->addView(image_box);
}

void rebuild_general_panel(const std::shared_ptr<SettingsDraftState>& state) {
    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();
    const auto& general = state->draft_config.general;

    const auto add_group_label = [&](const std::string& text) {
        auto* label = create_label(text, 17.0f, theme["brls/text_disabled"], true);
        label->setMargins(6, 6, 10, 0);
        container->addView(label);
    };

    const auto add_setting_cell = [&](const std::string& view_id,
                                      const std::string& title,
                                      const std::string& value,
                                      std::function<bool(brls::View*)> action) {
        container->addView(create_action_cell(
            title,
            summarize_detail_text(value, 44),
            theme["brls/list/listItem_value_color"],
            std::move(action),
            view_id));
    };

    add_group_label(tr("settings_page/general/groups/basic"));

    add_setting_cell(
        "settings/general/language",
        tr("settings_page/language/title"),
        selected_language_display_name(state),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/language");
            open_language_dropdown(state);
            return true;
        });

    add_setting_cell(
        "settings/general/touch_enable",
        tr("settings_page/general/touch_enable/title"),
        bool_display_text(general.touch_enable),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/touch_enable");
            toggle_touch_enable(state);
            return true;
        });

    add_setting_cell(
        "settings/general/exit_to_home_screen",
        tr("settings_page/general/exit_to_home_screen/title"),
        bool_display_text(general.exit_to_home_screen),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/exit_to_home_screen");
            toggle_exit_to_home_screen(state);
            return true;
        });

    add_setting_cell(
        "settings/general/playable_extensions",
        tr("settings_page/general/playable_extensions/title"),
        general.playable_extensions,
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/playable_extensions");
            open_playable_extensions_editor(state);
            return true;
        });

    add_setting_cell(
        "settings/general/sort_order",
        tr("settings_page/general/sort_order/title"),
        sort_order_display_name(general.sort_order),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/sort_order");
            open_sort_order_dropdown(state);
            return true;
        });

    add_group_label(tr("settings_page/general/groups/playback"));

    add_setting_cell(
        "settings/general/hardware_decode",
        tr("settings_page/general/hardware_decode/title"),
        bool_display_text(general.hardware_decode),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/hardware_decode");
            toggle_hardware_decode(state);
            return true;
        });

    add_setting_cell(
        "settings/general/short_seek",
        tr("settings_page/general/short_seek/title"),
        tr("settings_page/general/seconds_value", std::to_string(general.short_seek)),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/short_seek");
            open_short_seek_editor(state);
            return true;
        });

    add_setting_cell(
        "settings/general/long_seek",
        tr("settings_page/general/long_seek/title"),
        tr("settings_page/general/seconds_value", std::to_string(general.long_seek)),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/long_seek");
            open_long_seek_editor(state);
            return true;
        });

    add_setting_cell(
        "settings/general/y_hold_speed_multiplier",
        tr("settings_page/general/y_hold_speed_multiplier/title"),
        tr("settings_page/general/multiplier_value", format_float_value(general.y_hold_speed_multiplier, 1)),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/y_hold_speed_multiplier");
            open_y_hold_speed_multiplier_editor(state);
            return true;
        });

    add_setting_cell(
        "settings/general/continuous_seek_interval_ms",
        tr("settings_page/general/continuous_seek_interval_ms/title"),
        tr(
            "settings_page/general/milliseconds_value",
            std::to_string(general.continuous_seek_interval_ms)),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/continuous_seek_interval_ms");
            open_continuous_seek_interval_editor(state);
            return true;
        });

    add_setting_cell(
        "settings/general/touch_player_gestures",
        tr("settings_page/general/touch_player_gestures/title"),
        bool_display_text(general.touch_player_gestures),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/touch_player_gestures");
            toggle_touch_player_gestures(state);
            return true;
        });

    add_setting_cell(
        "settings/general/use_preferred_audio_language",
        tr("settings_page/general/use_preferred_audio_language/title"),
        bool_display_text(general.use_preferred_audio_language),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/use_preferred_audio_language");
            toggle_preferred_audio_language(state);
            return true;
        });

    add_setting_cell(
        "settings/general/preferred_audio_language",
        tr("settings_page/general/preferred_audio_language/title"),
        preferred_language_display_text(general.preferred_audio_language),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/preferred_audio_language");
            open_preferred_audio_language_picker(state);
            return true;
        });

    add_setting_cell(
        "settings/general/use_preferred_subtitle_language",
        tr("settings_page/general/use_preferred_subtitle_language/title"),
        bool_display_text(general.use_preferred_subtitle_language),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/use_preferred_subtitle_language");
            toggle_preferred_subtitle_language(state);
            return true;
        });

    add_setting_cell(
        "settings/general/preferred_subtitle_language",
        tr("settings_page/general/preferred_subtitle_language/title"),
        preferred_language_display_text(general.preferred_subtitle_language),
        [state](brls::View*) {
            request_focus_restore(state, "settings/general/preferred_subtitle_language");
            open_preferred_subtitle_language_picker(state);
            return true;
        });
}

void rebuild_iptv_panel(const std::shared_ptr<SettingsDraftState>& state) {
    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();

    for (const auto& source : state->draft_config.iptv_sources) {
        auto* cell = create_action_cell(
            status_icon_title(source.enabled, source.title),
            summarize_iptv_source(source),
            source.enabled ? theme["brls/list/listItem_value_color"] : theme["brls/text_disabled"],
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_iptv_source(state, key)) {
                    open_iptv_editor(state, *selected);
                }
                return true;
            },
            make_iptv_source_view_id(source.key));
        cell->registerAction(
            source.enabled ? tr("actions/disable") : tr("actions/enable"),
            brls::BUTTON_Y,
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_iptv_source(state, key)) {
                    request_focus_restore(state, make_iptv_source_view_id(key));
                    selected->enabled = !selected->enabled;
                    sync_dirty_state(state);
                    rebuild_right_panel(state);
                    return true;
                }
                return false;
            },
            false,
            false,
            brls::SOUND_CLICK);
        cell->registerAction(
            tr("actions/delete"),
            brls::BUTTON_X,
            [state, key = source.key](brls::View*) {
                auto* selected = find_iptv_source(state, key);
                if (selected == nullptr) {
                    return false;
                }

                confirm_delete_entry(
                    tr("settings_page/iptv/delete_confirm", visible_entry_title(selected->title)),
                    [state, key]() {
                        remove_iptv_source(state, key);
                    });
                return true;
            },
            false,
            false,
            brls::SOUND_CLICK);
        container->addView(cell);
    }

    if (state->draft_config.iptv_sources.empty()) {
        auto* empty_label = create_label(
            tr("settings_page/iptv/list_empty"),
            17.0f,
            theme["brls/text_disabled"]);
        empty_label->setMargins(8, 14, 14, 14);
        container->addView(empty_label);
    }

    container->addView(create_action_cell(
        tr("settings_page/iptv/add_entry"),
        tr("settings_page/iptv/add_entry_detail"),
        theme["brls/highlight/color2"],
        [state](brls::View*) {
            switchbox::core::IptvSourceSettings source;
            source.key = make_unique_iptv_key(state);
            source.title = tr("settings_page/iptv/default_title");
            open_iptv_editor(state, std::move(source));
            return true;
        }));
}

void rebuild_smb_panel(const std::shared_ptr<SettingsDraftState>& state) {
    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();

    for (const auto& source : state->draft_config.smb_sources) {
        auto* cell = create_action_cell(
            status_icon_title(source.enabled, source.title),
            summarize_smb_source(source),
            source.enabled ? theme["brls/list/listItem_value_color"] : theme["brls/text_disabled"],
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_smb_source(state, key)) {
                    open_smb_editor(state, *selected);
                }
                return true;
            },
            make_smb_source_view_id(source.key));
        cell->registerAction(
            source.enabled ? tr("actions/disable") : tr("actions/enable"),
            brls::BUTTON_Y,
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_smb_source(state, key)) {
                    request_focus_restore(state, make_smb_source_view_id(key));
                    selected->enabled = !selected->enabled;
                    sync_dirty_state(state);
                    rebuild_right_panel(state);
                    return true;
                }
                return false;
            },
            false,
            false,
            brls::SOUND_CLICK);
        cell->registerAction(
            tr("actions/delete"),
            brls::BUTTON_X,
            [state, key = source.key](brls::View*) {
                auto* selected = find_smb_source(state, key);
                if (selected == nullptr) {
                    return false;
                }

                confirm_delete_entry(
                    tr("settings_page/smb/delete_confirm", visible_entry_title(selected->title)),
                    [state, key]() {
                        remove_smb_source(state, key);
                    });
                return true;
            },
            false,
            false,
            brls::SOUND_CLICK);
        container->addView(cell);
    }

    if (state->draft_config.smb_sources.empty()) {
        auto* empty_label = create_label(
            tr("settings_page/smb/list_empty"),
            17.0f,
            theme["brls/text_disabled"]);
        empty_label->setMargins(8, 14, 14, 14);
        container->addView(empty_label);
    }

    container->addView(create_action_cell(
        tr("settings_page/smb/add_entry"),
        tr("settings_page/smb/add_entry_detail"),
        theme["brls/highlight/color2"],
        [state](brls::View*) {
            switchbox::core::SmbSourceSettings source;
            source.key = make_unique_smb_key(state);
            source.title = tr("settings_page/smb/default_title");
            open_smb_editor(state, std::move(source));
            return true;
        }));
}

void rebuild_right_panel(const std::shared_ptr<SettingsDraftState>& state) {
    if (state->right_content_box == nullptr) {
        return;
    }

    if (state->right_scrolling_frame != nullptr) {
        const bool interactive_section =
            state->active_section != SettingsSection::Donate &&
            state->active_section != SettingsSection::Update;
        state->right_scrolling_frame->setFocusable(interactive_section);
        if (auto* static_safe_frame = dynamic_cast<StaticSafeScrollingFrame*>(state->right_scrolling_frame)) {
            static_safe_frame->setTouchHitTestEnabled(interactive_section);
        }
    }

    sync_dirty_state(state);
    const std::string focus_restore_id = state->focus_restore_id;
    state->focus_restore_id.clear();

    state->right_content_box->clearViews();
    state->right_content_box->setJustifyContent(brls::JustifyContent::FLEX_START);

    switch (state->active_section) {
        case SettingsSection::Donate:
            rebuild_donate_panel(state);
            break;
        case SettingsSection::General:
            rebuild_general_panel(state);
            break;
        case SettingsSection::Update:
            rebuild_update_panel(state);
            break;
        case SettingsSection::Iptv:
            rebuild_iptv_panel(state);
            break;
        case SettingsSection::Smb:
            rebuild_smb_panel(state);
            break;
    }

    if (!focus_restore_id.empty()) {
        if (auto* target = state->right_content_box->getView(focus_restore_id)) {
            brls::Application::giveFocus(target);
        }
    }
}

brls::View* create_settings_content(const std::shared_ptr<SettingsDraftState>& state) {
    state->saved_config = switchbox::core::AppConfigStore::current();
    state->draft_config = state->saved_config;

    auto* root = new brls::Box(brls::Axis::ROW);

    auto* sidebar = new brls::Sidebar();
    sidebar->setWidth(260);
    sidebar->setGrow(0);
    state->sidebar = sidebar;
    sidebar->addItem(
        tr("settings_page/general/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::General);
        });
    sidebar->addItem(
        tr("settings_page/update/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::Update);
        });
    sidebar->addItem(
        tr("settings_page/donate/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::Donate);
        });
    sidebar->addItem(
        tr("sections/iptv/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::Iptv);
        });
    sidebar->addItem(
        tr("sections/smb/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::Smb);
        });
    if (auto& sidebar_children = sidebar->getChildren(); !sidebar_children.empty()) {
        if (auto* sidebar_content = dynamic_cast<brls::Box*>(sidebar_children.back())) {
            sidebar_content->setDefaultFocusedIndex(0);
        }
    }
    root->addView(sidebar);

    auto* right_content = new brls::Box(brls::Axis::COLUMN);
    right_content->setPadding(24, 40, 24, 40);
    state->right_content_box = right_content;

    auto* right_frame = new StaticSafeScrollingFrame();
    right_frame->setContentView(right_content);
    right_frame->setGrow(1.0f);
    state->right_scrolling_frame = right_frame;
    root->addView(right_frame);

    rebuild_right_panel(state);

    auto* frame = new brls::AppletFrame(root);
    configure_applet_frame(frame, tr("sections/settings/title"));
    frame->registerAction(
        tr("actions/save"),
        brls::BUTTON_START,
        [state](brls::View*) {
            return apply_draft_changes(state);
        },
        false,
        false,
        brls::SOUND_CLICK);
    frame->registerAction(
        brls::getStr("hints/back"),
        brls::BUTTON_B,
        [state](brls::View*) {
            return exit_settings_with_confirm_if_needed(state);
        },
        false,
        false,
        brls::SOUND_CLICK);
    state->ui_ready = true;
    brls::Application::getGlobalHintsUpdateEvent()->fire();
    return frame;
}

}  // namespace

SettingsActivity::SettingsActivity()
    : brls::Activity()
    , state(std::make_shared<SettingsDraftState>()) {}

brls::View* SettingsActivity::createContentView() {
    return create_settings_content(this->state);
}

void SettingsActivity::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    schedule_update_check(this->state);
}

SettingsActivity::~SettingsActivity() {
    if (this->state == nullptr) {
        return;
    }

    this->state->ui_ready = false;
    this->state->sidebar = nullptr;
    this->state->right_content_box = nullptr;
    this->state->right_scrolling_frame = nullptr;
    if (this->state->update_check_cancel_flag != nullptr) {
        this->state->update_check_cancel_flag->store(true);
    }
}

}  // namespace switchbox::app
