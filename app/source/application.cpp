#include "switchbox/app/application.hpp"

#include <cstdlib>

#include <borealis.hpp>

#include "switchbox/app/config_missing_activity.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/core/language.hpp"
#include "switchbox/app/home_activity.hpp"

namespace switchbox::app {

int Application::run(const StartupContext& context) const {
    switchbox::core::AppConfigStore::set_runtime_executable_path(context.executable_path);
    bool configReady = switchbox::core::AppConfigStore::initialize();
    switchbox::core::LanguageState languageState{};
    const auto& paths = switchbox::core::AppConfigStore::paths();

    brls::setTranslationSearchPaths({paths.languages_directory.string()});

    if (configReady) {
        const auto& config = switchbox::core::AppConfigStore::current();
        languageState = switchbox::core::resolve_language_state(paths, config);

        if (languageState.using_auto) {
            brls::Application::clearLocaleOverride();
        } else {
            brls::Application::setLocaleOverride(languageState.active_language);
        }
    }

    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    if (!configReady) {
        brls::Logger::warning("Unable to initialize config store");
        for (const auto& candidate : paths.config_search_candidates) {
            brls::Logger::warning("Config search miss: {}", candidate.string());
        }
    } else {
        brls::Logger::info(
            "Config ready: file={}, langs={}",
            paths.config_file.string(),
            paths.languages_directory.string());
        brls::Logger::info(
            "Language ready: configured={}, active={}, auto={}",
            languageState.configured_language.empty() ? "auto" : languageState.configured_language,
            brls::Application::getLocale(),
            languageState.using_auto ? "true" : "false");
    }

    brls::Application::createWindow(switchbox::core::BuildInfo::app_name());
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_height", 64.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_padding_top_bottom", 8.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_padding_sides", 26.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_title_font_size", 22.0f);
    brls::Application::getStyle().addMetric("brls/applet_frame/header_title_top_offset", 3.0f);
    brls::Application::setGlobalQuit(false);

    if (!configReady && context.switch_target) {
        brls::Application::pushActivity(new ConfigMissingActivity(paths.config_search_candidates));
    } else {
        brls::Application::pushActivity(new HomeActivity(context));
    }

    while (brls::Application::mainLoop()) {
    }

    return EXIT_SUCCESS;
}

}  // namespace switchbox::app
