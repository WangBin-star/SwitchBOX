#include "switchbox/app/settings_activity.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/cells/cell_selector.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include "switchbox/app/header_status_hint.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/language.hpp"

namespace switchbox::app {

namespace {

struct LanguageOption {
    std::string value;
    std::string label;
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

void apply_native_status_layout(brls::AppletFrame* frame) {
    frame->setTitle(tr("sections/settings/title"));

    if (auto* time_view = frame->getView("brls/hints/time")) {
        time_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* wireless_view = frame->getView("brls/wireless")) {
        wireless_view->setVisibility(brls::Visibility::GONE);
    }

    if (auto* battery_view = frame->getView("brls/battery")) {
        battery_view->setVisibility(brls::Visibility::GONE);
    }
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
    const switchbox::core::LanguageState& language_state) {
    const std::string selected_value =
        language_state.using_auto ? "auto" : language_state.active_language;

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

brls::View* create_settings_content() {
    auto theme = brls::Application::getTheme();
    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto& config = switchbox::core::AppConfigStore::current();
    const auto language_state = switchbox::core::resolve_language_state(paths, config);
    const std::vector<LanguageOption> options = build_language_options(language_state);
    const int initial_selection = find_language_selection(options, language_state);
    const std::string active_language =
        language_state.using_auto ? brls::Application::getLocale() : language_state.active_language;

    auto* container = new brls::Box(brls::Axis::COLUMN);
    container->setPadding(30, 40, 30, 40);

    auto* eyebrow = create_label(
        tr("settings_page/eyebrow"),
        15.0f,
        theme["brls/highlight/color2"],
        true);
    eyebrow->setMargins(0, 0, 8, 0);
    container->addView(eyebrow);

    auto* header = new brls::Header();
    header->setTitle(tr("sections/settings/title"));
    header->setSubtitle(tr("settings_page/header_subtitle"));
    container->addView(header);

    auto* summary = create_label(
        tr("settings_page/summary"),
        18.0f,
        theme["brls/text"]);
    summary->setMargins(0, 0, 12, 0);
    container->addView(summary);

    auto* section_label = create_label(
        tr("settings_page/general/title"),
        24.0f,
        theme["brls/text"],
        true);
    section_label->setMargins(14, 0, 4, 0);
    container->addView(section_label);

    auto* section_summary = create_label(
        tr("settings_page/general/summary"),
        16.0f,
        theme["brls/header/subtitle"]);
    section_summary->setMargins(0, 0, 10, 0);
    container->addView(section_summary);

    auto* language_selector = new brls::SelectorCell();
    std::vector<std::string> option_labels;
    option_labels.reserve(options.size());
    for (const auto& option : options) {
        option_labels.push_back(option.label);
    }

    int last_selection = initial_selection;
    language_selector->init(
        tr("settings_page/language/title"),
        option_labels,
        initial_selection,
        [language_selector, options, last_selection](int selection) mutable {
            if (selection < 0 || selection >= static_cast<int>(options.size())) {
                return;
            }

            auto& mutable_config = switchbox::core::AppConfigStore::mutable_config();
            const std::string previous_language = mutable_config.general.language;
            mutable_config.general.language = options[selection].value;

            if (switchbox::core::AppConfigStore::save()) {
                last_selection = selection;
                brls::Application::notify(tr("settings_page/language/save_success"));
                return;
            }

            mutable_config.general.language = previous_language;
            language_selector->setSelection(last_selection, true);
            brls::Application::notify(tr("settings_page/language/save_failed"));
        });
    container->addView(language_selector);

    container->addView(create_info_cell(
        tr("settings_page/language/current_title"),
        language_display_name(active_language),
        theme["brls/list/listItem_value_color"]));
    container->addView(create_info_cell(
        tr("settings_page/language/config_title"),
        paths.config_file.string(),
        theme["brls/text"]));

    auto* scrolling_frame = new brls::ScrollingFrame();
    scrolling_frame->setContentView(container);
    scrolling_frame->getAppletFrameItem()->setHintView(new HeaderStatusHint());

    auto* frame = new brls::AppletFrame(scrolling_frame);
    apply_native_status_layout(frame);
    return frame;
}

}  // namespace

SettingsActivity::SettingsActivity()
    : brls::Activity(create_settings_content()) {
    registerExitAction();
}

}  // namespace switchbox::app
