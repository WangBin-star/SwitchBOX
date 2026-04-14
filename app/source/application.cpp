#include "switchbox/app/application.hpp"

#include <cstdlib>

#include <borealis.hpp>
#include <borealis/core/thread.hpp>

#include "switchbox/app/config_missing_activity.hpp"
#include "switchbox/app/settings_activity.hpp"
#include "switchbox/core/app_config.hpp"
#include "switchbox/core/build_info.hpp"
#include "switchbox/core/language.hpp"
#include "switchbox/app/home_activity.hpp"

namespace switchbox::app {

namespace {

StartupContext g_runtime_context{};

void apply_language_state(const switchbox::core::LanguageState& language_state) {
    if (language_state.using_auto) {
        brls::Application::clearLocaleOverride();
    } else {
        brls::Application::setLocaleOverride(language_state.active_language);
    }

    brls::reloadTranslations();
}

void rebuild_root_ui(const StartupContext& context, bool reopen_settings) {
    const bool config_ready = switchbox::core::AppConfigStore::loaded_from_disk();
    const auto& paths = switchbox::core::AppConfigStore::paths();

    brls::Application::clearActivities();

    if (!config_ready && context.switch_target) {
        brls::Application::pushActivity(new ConfigMissingActivity(paths.config_search_candidates));
        return;
    }

    brls::Application::pushActivity(new HomeActivity(context));

    if (reopen_settings) {
        brls::Application::pushActivity(new SettingsActivity());
    }
}

}  // namespace

void Application::set_runtime_context(const StartupContext& context) {
    g_runtime_context = context;
}

const StartupContext& Application::runtime_context() {
    return g_runtime_context;
}

void Application::reload_root_ui(bool reopen_settings) {
    brls::delay(0, [reopen_settings]() {
        rebuild_root_ui(g_runtime_context, reopen_settings);
    });
}

void Application::apply_language_and_reload_ui(bool reopen_settings) {
    const auto& paths = switchbox::core::AppConfigStore::paths();
    const auto& config = switchbox::core::AppConfigStore::current();
    const auto language_state = switchbox::core::resolve_language_state(paths, config);

    apply_language_state(language_state);
    Application::reload_root_ui(reopen_settings);
}

int Application::run(const StartupContext& context) const {
    Application::set_runtime_context(context);
    switchbox::core::AppConfigStore::set_runtime_executable_path(context.executable_path);
    bool configReady = switchbox::core::AppConfigStore::initialize();
    switchbox::core::LanguageState languageState{};
    const auto& paths = switchbox::core::AppConfigStore::paths();

    brls::setTranslationSearchPaths({paths.languages_directory.string()});

    if (configReady) {
        const auto& config = switchbox::core::AppConfigStore::current();
        languageState = switchbox::core::resolve_language_state(paths, config);
        apply_language_state(languageState);
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
