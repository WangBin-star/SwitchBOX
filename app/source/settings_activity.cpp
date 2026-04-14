#include "switchbox/app/settings_activity.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_bool.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/cells/cell_input.hpp>
#include <borealis/views/dropdown.hpp>
#include <borealis/views/hint.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>
#include <borealis/views/sidebar.hpp>

#include "switchbox/app/application.hpp"
#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/language.hpp"

namespace switchbox::app {

namespace {

enum class SettingsSection {
    General,
    Iptv,
    Smb,
};

struct LanguageOption {
    std::string value;
    std::string label;
};

struct SettingsDraftState {
    switchbox::core::AppConfig saved_config;
    switchbox::core::AppConfig draft_config;
    SettingsSection active_section = SettingsSection::General;
    bool dirty = false;
    brls::Sidebar* sidebar = nullptr;
    brls::Box* right_content_box = nullptr;
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

brls::DetailCell* create_info_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detail_color) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->setDetailTextColor(detail_color);
    return cell;
}

brls::DetailCell* create_action_cell(
    const std::string& title,
    const std::string& detail,
    NVGcolor detail_color,
    std::function<bool(brls::View*)> action) {
    auto* cell = create_info_cell(title, detail, detail_color);
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

    if (auto* content = frame->getContentView()) {
        content->getAppletFrameItem()->setHintView(new HeaderStatusHint());
    }

    if (auto* time_view = frame->getView("brls/hints/time")) {
        time_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* wireless_view = frame->getView("brls/wireless")) {
        wireless_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* battery_view = frame->getView("brls/battery")) {
        battery_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* hints = dynamic_cast<brls::Hints*>(frame->getView("brls/hints"))) {
        hints->setAllowAButtonTouch(true);
    }
}

bool general_settings_equal(
    const switchbox::core::GeneralSettings& lhs,
    const switchbox::core::GeneralSettings& rhs) {
    return lhs.language == rhs.language &&
           lhs.playable_extensions == rhs.playable_extensions;
}

bool iptv_source_equal(
    const switchbox::core::IptvSourceSettings& lhs,
    const switchbox::core::IptvSourceSettings& rhs) {
    return lhs.key == rhs.key &&
           lhs.title == rhs.title &&
           lhs.url == rhs.url &&
           lhs.enabled == rhs.enabled;
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
           lhs.enabled == rhs.enabled;
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

std::string language_display_name(const std::string& locale) {
    if (locale == "en-US") {
        return tr("settings_page/language/names/en-US");
    }

    if (locale == "zh-Hans") {
        return tr("settings_page/language/names/zh-Hans");
    }

    if (locale == "zh-Hant") {
        return tr("settings_page/language/names/zh-Hant");
    }

    if (locale == "ja") {
        return tr("settings_page/language/names/ja");
    }

    if (locale == "ko") {
        return tr("settings_page/language/names/ko");
    }

    if (locale == "fr") {
        return tr("settings_page/language/names/fr");
    }

    if (locale == "ru") {
        return tr("settings_page/language/names/ru");
    }

    return locale;
}

std::vector<LanguageOption> build_language_options(const switchbox::core::LanguageState& language_state) {
    std::vector<LanguageOption> options;
    options.push_back({
        .value = "auto",
        .label = tr(
            "settings_page/language/options/auto",
            language_display_name(brls::Application::getLocale())),
    });

    for (const auto& locale : language_state.available_languages) {
        options.push_back({
            .value = locale,
            .label = language_display_name(locale),
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
    const std::string effective_language =
        state->draft_config.general.language == "auto" ? brls::Application::getLocale()
                                                       : state->draft_config.general.language;
    return language_display_name(effective_language);
}

std::string visible_entry_title(const std::string& title) {
    if (!title.empty()) {
        return title;
    }

    return tr("settings_page/common/untitled_entry");
}

std::string status_prefixed_title(bool enabled, const std::string& title) {
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

void sync_dirty_state(const std::shared_ptr<SettingsDraftState>& state) {
    state->dirty = !app_config_equal(state->draft_config, state->saved_config);
    brls::Application::getGlobalHintsUpdateEvent()->fire();
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
    state->active_section = section;
    rebuild_right_panel(state);
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

void open_language_dropdown(const std::shared_ptr<SettingsDraftState>& state) {
    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto language_state = switchbox::core::resolve_language_state(paths, state->draft_config);
    const std::vector<LanguageOption> options = build_language_options(language_state);
    std::vector<std::string> option_labels;
    option_labels.reserve(options.size());
    for (const auto& option : options) {
        option_labels.push_back(option.label);
    }

    const int selected_index = find_language_selection(options, state->draft_config.general.language);
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

void rebuild_general_panel(const std::shared_ptr<SettingsDraftState>& state) {
    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();

    container->addView(create_action_cell(
        tr("settings_page/language/title"),
        selected_language_display_name(state),
        theme["brls/list/listItem_value_color"],
        [state](brls::View*) {
            open_language_dropdown(state);
            return true;
        }));

    container->addView(create_action_cell(
        tr("settings_page/general/playable_extensions/title"),
        summarize_detail_text(state->draft_config.general.playable_extensions),
        theme["brls/list/listItem_value_color"],
        [state](brls::View*) {
            open_playable_extensions_editor(state);
            return true;
        }));
}

void rebuild_iptv_panel(const std::shared_ptr<SettingsDraftState>& state) {
    auto* container = state->right_content_box;
    auto theme = brls::Application::getTheme();

    for (const auto& source : state->draft_config.iptv_sources) {
        auto* cell = create_action_cell(
            status_prefixed_title(source.enabled, source.title),
            summarize_iptv_source(source),
            source.enabled ? theme["brls/list/listItem_value_color"] : theme["brls/text_disabled"],
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_iptv_source(state, key)) {
                    open_iptv_editor(state, *selected);
                }
                return true;
            });
        cell->registerAction(
            source.enabled ? tr("actions/disable") : tr("actions/enable"),
            brls::BUTTON_Y,
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_iptv_source(state, key)) {
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
            status_prefixed_title(source.enabled, source.title),
            summarize_smb_source(source),
            source.enabled ? theme["brls/list/listItem_value_color"] : theme["brls/text_disabled"],
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_smb_source(state, key)) {
                    open_smb_editor(state, *selected);
                }
                return true;
            });
        cell->registerAction(
            source.enabled ? tr("actions/disable") : tr("actions/enable"),
            brls::BUTTON_Y,
            [state, key = source.key](brls::View*) {
                if (auto* selected = find_smb_source(state, key)) {
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

    sync_dirty_state(state);

    brls::View* current_focus = brls::Application::getCurrentFocus();
    while (current_focus != nullptr) {
        if (current_focus == state->right_content_box) {
            if (state->sidebar != nullptr) {
                brls::Application::giveFocus(state->sidebar);
            }
            break;
        }
        current_focus = current_focus->getParent();
    }

    state->right_content_box->clearViews();

    switch (state->active_section) {
        case SettingsSection::General:
            rebuild_general_panel(state);
            break;
        case SettingsSection::Iptv:
            rebuild_iptv_panel(state);
            break;
        case SettingsSection::Smb:
            rebuild_smb_panel(state);
            break;
    }
}

brls::View* create_settings_content() {
    auto state = std::make_shared<SettingsDraftState>();
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
        tr("sections/iptv/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::Iptv);
        });
    sidebar->addItem(
        tr("sections/smb/title"),
        [state](brls::View*) {
            select_section(state, SettingsSection::Smb);
        });
    root->addView(sidebar);

    auto* right_content = new brls::Box(brls::Axis::COLUMN);
    right_content->setPadding(24, 40, 24, 40);
    state->right_content_box = right_content;

    auto* right_frame = new brls::ScrollingFrame();
    right_frame->setContentView(right_content);
    right_frame->setGrow(1.0f);
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

    return frame;
}

}  // namespace

SettingsActivity::SettingsActivity()
    : brls::Activity(create_settings_content()) {
    registerExitAction();
}

}  // namespace switchbox::app
