#include "switchbox/core/language.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string_view>

namespace switchbox::core {

namespace {

constexpr std::string_view kAutoLanguage = "auto";
constexpr std::string_view kDefaultLanguage = "en-US";

std::string trim(std::string value) {
    const auto isSpace = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t index = 0; index < left.size(); index++) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }

    return true;
}

std::string normalize_language_tag(std::string value) {
    value = trim(std::move(value));

    if (value.empty()) {
        return {};
    }

    std::replace(value.begin(), value.end(), '_', '-');

    if (equals_ignore_case(value, kAutoLanguage)) {
        return std::string(kAutoLanguage);
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
        return to_lower(std::move(value));
    }

    if (value.size() >= 5 && value[2] == '-') {
        std::string normalized;
        normalized.reserve(value.size());
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[0]))));
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[1]))));
        normalized.push_back('-');

        for (size_t index = 3; index < value.size(); index++) {
            const unsigned char character = static_cast<unsigned char>(value[index]);
            normalized.push_back(static_cast<char>(index == 3 ? std::toupper(character) : std::tolower(character)));
        }

        return normalized;
    }

    return value;
}

bool has_json_files(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || !std::filesystem::is_directory(directory, error)) {
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }

        if (entry.is_regular_file(error) && entry.path().extension() == ".json") {
            return true;
        }
    }

    return false;
}

std::string resolve_explicit_language(
    const std::string& configuredLanguage,
    const std::vector<std::string>& availableLanguages) {
    const std::string normalized = normalize_language_tag(configuredLanguage);
    if (normalized.empty() || normalized == kAutoLanguage) {
        return {};
    }

    for (const auto& language : availableLanguages) {
        if (equals_ignore_case(language, normalized)) {
            return language;
        }
    }

    const size_t separator = normalized.find('-');
    if (separator != std::string::npos) {
        const std::string baseLanguage = normalized.substr(0, separator);
        for (const auto& language : availableLanguages) {
            if (equals_ignore_case(language, baseLanguage)) {
                return language;
            }
        }
    }

    return std::string(kDefaultLanguage);
}

}  // namespace

std::vector<std::string> collect_available_languages(const AppPaths& paths) {
    std::set<std::string> languages{
        "en-US",
        "zh-Hans",
    };

    std::error_code error;
    if (std::filesystem::exists(paths.languages_directory, error) &&
        std::filesystem::is_directory(paths.languages_directory, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(paths.languages_directory, error)) {
            if (error || !entry.is_directory(error)) {
                continue;
            }

            if (!has_json_files(entry.path())) {
                continue;
            }

            const std::string normalized = normalize_language_tag(entry.path().filename().string());
            if (!normalized.empty() && normalized != kAutoLanguage) {
                languages.insert(normalized);
            }
        }
    }

    return {languages.begin(), languages.end()};
}

LanguageState resolve_language_state(const AppPaths& paths, const AppConfig& config) {
    LanguageState state;
    state.configured_language = normalize_language_tag(config.general.language);
    state.using_auto = state.configured_language.empty() || state.configured_language == kAutoLanguage;
    state.available_languages = collect_available_languages(paths);
    state.active_language = state.using_auto
        ? std::string(kAutoLanguage)
        : resolve_explicit_language(state.configured_language, state.available_languages);

    if (!state.using_auto && state.active_language.empty()) {
        state.active_language = std::string(kDefaultLanguage);
    }

    return state;
}

}  // namespace switchbox::core
