#include "switchbox/core/app_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string_view>
#include <system_error>

namespace switchbox::core {

namespace {

struct StoreState {
    AppPaths paths;
    AppConfig config;
    bool initialized = false;
    bool loadedFromDisk = false;
    std::filesystem::path runtimeExecutablePath;
};

StoreState& state() {
    static StoreState instance;
    return instance;
}

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

bool parse_bool(const std::string& value, bool fallback) {
    const std::string normalized = to_lower(trim(value));

    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }

    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }

    return fallback;
}

AppPaths make_paths(const std::filesystem::path& baseDirectory) {
    return {
        .base_directory = baseDirectory,
        .config_file = baseDirectory / "switchbox.ini",
        .languages_directory = baseDirectory / "langs",
        .config_search_candidates = {},
    };
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

void append_candidate(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return;
    }

    const std::filesystem::path normalized = candidate.lexically_normal();
    const auto duplicate = std::find(candidates.begin(), candidates.end(), normalized);
    if (duplicate == candidates.end()) {
        candidates.push_back(normalized);
    }
}

AppPaths resolve_paths() {
#ifdef __SWITCH__
    auto& store = state();
    std::vector<std::filesystem::path> baseCandidates;
    append_candidate(baseCandidates, std::filesystem::path("sdmc:/switch/SwitchBOX"));
    append_candidate(baseCandidates, store.runtimeExecutablePath.parent_path());

    AppPaths resolved = make_paths(baseCandidates.empty() ? std::filesystem::path("sdmc:/switch/SwitchBOX")
                                                          : baseCandidates.front());

    for (const auto& baseCandidate : baseCandidates) {
        const auto candidatePaths = make_paths(baseCandidate);
        resolved.config_search_candidates.push_back(candidatePaths.config_file);

        if (path_exists(candidatePaths.config_file)) {
            resolved = candidatePaths;
            resolved.config_search_candidates = {};
            for (const auto& candidate : baseCandidates) {
                resolved.config_search_candidates.push_back(make_paths(candidate).config_file);
            }
            return resolved;
        }
    }

    return resolved;
#else
    const std::filesystem::path baseDirectory = std::filesystem::current_path();
    AppPaths paths = make_paths(baseDirectory);
    paths.config_search_candidates.push_back(paths.config_file);
    return paths;
#endif
}

using IniSection = std::map<std::string, std::string>;
using IniDocument = std::map<std::string, IniSection>;

IniDocument parse_ini(const std::filesystem::path& path) {
    IniDocument document;
    std::ifstream input(path);

    if (!input.is_open()) {
        return document;
    }

    std::string currentSection;
    std::string line;

    while (std::getline(input, line)) {
        line = trim(line);

        if (line.empty() || line.starts_with(';') || line.starts_with('#')) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            currentSection = trim(line.substr(1, line.size() - 2));
            continue;
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));

        if (!key.empty()) {
            document[currentSection][key] = value;
        }
    }

    return document;
}

std::string get_value(
    const IniDocument& document,
    std::string_view section,
    std::string_view key,
    const std::string& fallback = "") {
    const auto sectionIt = document.find(std::string(section));
    if (sectionIt == document.end()) {
        return fallback;
    }

    const auto valueIt = sectionIt->second.find(std::string(key));
    if (valueIt == sectionIt->second.end()) {
        return fallback;
    }

    return valueIt->second;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

void load_config_from_document(const IniDocument& document, AppConfig& config) {
    config.general.language =
        get_value(document, "general", "language", config.general.language);

    config.iptv_sources.clear();
    config.smb_sources.clear();

    for (const auto& [sectionName, section] : document) {
        if (starts_with(sectionName, "iptv-")) {
            IptvSourceSettings source;
            source.key = sectionName.substr(5);
            source.title = get_value(document, sectionName, "title", source.key);
            source.url = get_value(document, sectionName, "url");
            source.enabled = parse_bool(
                get_value(document, sectionName, "enabled"),
                source.enabled);
            config.iptv_sources.push_back(std::move(source));
            continue;
        }

        if (starts_with(sectionName, "smb-")) {
            SmbSourceSettings source;
            source.key = sectionName.substr(4);
            source.title = get_value(document, sectionName, "title", source.key);
            source.host = get_value(document, sectionName, "host");
            source.share = get_value(document, sectionName, "share");
            source.username = get_value(document, sectionName, "username");
            source.password = get_value(document, sectionName, "password");
            source.base_path = get_value(document, sectionName, "base_path");
            source.enabled = parse_bool(
                get_value(document, sectionName, "enabled"),
                source.enabled);
            config.smb_sources.push_back(std::move(source));
        }
    }

    if (config.iptv_sources.empty() && document.contains("iptv")) {
        IptvSourceSettings legacySource;
        legacySource.key = "default";
        legacySource.title = get_value(document, "iptv", "primary_source_name", legacySource.key);
        legacySource.url = get_value(document, "iptv", "primary_source_url");
        legacySource.enabled = parse_bool(
            get_value(document, "iptv", "primary_source_enabled"),
            legacySource.enabled);
        if (!legacySource.url.empty() || !legacySource.title.empty()) {
            config.iptv_sources.push_back(std::move(legacySource));
        }
    }

    if (config.smb_sources.empty() && document.contains("smb")) {
        SmbSourceSettings legacySource;
        legacySource.key = "default";
        legacySource.title = legacySource.key;
        legacySource.host = get_value(document, "smb", "host");
        legacySource.share = get_value(document, "smb", "share");
        legacySource.username = get_value(document, "smb", "username");
        legacySource.password = get_value(document, "smb", "password");
        legacySource.base_path = get_value(document, "smb", "base_path");
        if (!legacySource.host.empty() || !legacySource.share.empty()) {
            config.smb_sources.push_back(std::move(legacySource));
        }
    }
}

bool write_config_file(const AppPaths& paths, const AppConfig& config) {
    std::error_code error;
    std::filesystem::create_directories(paths.base_directory, error);

    std::ofstream output(paths.config_file, std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    output << "; SwitchBOX runtime configuration" << '\n';
    output << "; langs/ is searched relative to this file directory" << '\n';
    output << '\n';

    output << "[general]" << '\n';
    output << "language=" << config.general.language << '\n';
    output << '\n';

    output << "; IPTV sources use sections named [iptv-xxx]" << '\n';
    output << "; Example:" << '\n';
    output << "; [iptv-main]" << '\n';
    output << "; title=Main IPTV" << '\n';
    output << "; url=http://example.com/playlist.m3u" << '\n';
    output << "; enabled=true" << '\n';
    output << '\n';

    for (const auto& source : config.iptv_sources) {
        if (source.key.empty()) {
            continue;
        }

        output << "[iptv-" << source.key << "]" << '\n';
        output << "title=" << source.title << '\n';
        output << "url=" << source.url << '\n';
        output << "enabled=" << (source.enabled ? "true" : "false") << '\n';
        output << '\n';
    }

    output << "; SMB sources use sections named [smb-xxx]" << '\n';
    output << "; Example:" << '\n';
    output << "; [smb-media]" << '\n';
    output << "; title=Home NAS" << '\n';
    output << "; host=192.168.1.10" << '\n';
    output << "; share=video" << '\n';
    output << "; username=user" << '\n';
    output << "; password=pass" << '\n';
    output << "; base_path=/movies" << '\n';
    output << "; enabled=true" << '\n';
    output << '\n';

    for (const auto& source : config.smb_sources) {
        if (source.key.empty()) {
            continue;
        }

        output << "[smb-" << source.key << "]" << '\n';
        output << "title=" << source.title << '\n';
        output << "host=" << source.host << '\n';
        output << "share=" << source.share << '\n';
        output << "username=" << source.username << '\n';
        output << "password=" << source.password << '\n';
        output << "base_path=" << source.base_path << '\n';
        output << "enabled=" << (source.enabled ? "true" : "false") << '\n';
        output << '\n';
    }

    return output.good();
}

}  // namespace

void AppConfigStore::set_runtime_executable_path(std::filesystem::path executable_path) {
    auto& store = state();
    store.runtimeExecutablePath = std::move(executable_path);
    store.initialized = false;
    store.loadedFromDisk = false;
}

bool AppConfigStore::initialize() {
    auto& store = state();
    if (store.initialized) {
        return true;
    }

    store.paths = resolve_paths();

    if (std::filesystem::exists(store.paths.config_file)) {
        const IniDocument document = parse_ini(store.paths.config_file);
        load_config_from_document(document, store.config);
        store.loadedFromDisk = true;
        store.initialized = true;
        if (document.contains("iptv") || document.contains("smb")) {
            write_config_file(store.paths, store.config);
        }
        return true;
    }

    store.loadedFromDisk = false;
#ifdef __SWITCH__
    store.initialized = false;
    return false;
#else
    store.initialized = write_config_file(store.paths, store.config);
    return store.initialized;
#endif
}

bool AppConfigStore::save() {
    auto& store = state();
    if (!store.initialized && !initialize()) {
        return false;
    }

    return write_config_file(store.paths, store.config);
}

const AppConfig& AppConfigStore::current() {
    initialize();
    return state().config;
}

AppConfig& AppConfigStore::mutable_config() {
    initialize();
    return state().config;
}

const AppPaths& AppConfigStore::paths() {
    initialize();
    return state().paths;
}

bool AppConfigStore::loaded_from_disk() {
    initialize();
    return state().loadedFromDisk;
}

}  // namespace switchbox::core
