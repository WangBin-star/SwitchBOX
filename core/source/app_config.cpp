#include "switchbox/core/app_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
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

int parse_int(const std::string& value, int fallback) {
    const std::string normalized = trim(value);
    if (normalized.empty()) {
        return fallback;
    }

    try {
        return std::stoi(normalized);
    } catch (...) {
        return fallback;
    }
}

float parse_float(const std::string& value, float fallback) {
    const std::string normalized = trim(value);
    if (normalized.empty()) {
        return fallback;
    }

    try {
        return std::stof(normalized);
    } catch (...) {
        return fallback;
    }
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

std::string read_utf8_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string text = buffer.str();

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    if (text.size() >= utf8Bom.size() &&
        std::memcmp(text.data(), utf8Bom.data(), utf8Bom.size()) == 0) {
        text.erase(0, utf8Bom.size());
    }

    return text;
}

IniDocument parse_ini(const std::filesystem::path& path) {
    IniDocument document;
    const std::string text = read_utf8_text_file(path);
    if (text.empty()) {
        return document;
    }

    std::string currentSection;
    std::string line;
    std::istringstream input(text);

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
    config.general.playable_extensions =
        get_value(document, "general", "playable_extensions", config.general.playable_extensions);
    config.general.show_hidden = parse_bool(
        get_value(document, "general", "show_hidden"),
        config.general.show_hidden);
    config.general.sort_order =
        get_value(document, "general", "sort_order", config.general.sort_order);
    config.general.hardware_decode = parse_bool(
        get_value(document, "general", "hardware_decode"),
        config.general.hardware_decode);
    config.general.short_seek = parse_int(
        get_value(document, "general", "short_seek"),
        config.general.short_seek);
    config.general.long_seek = parse_int(
        get_value(document, "general", "long_seek"),
        config.general.long_seek);
    config.general.y_hold_speed_multiplier = parse_float(
        get_value(document, "general", "y_hold_speed_multiplier"),
        config.general.y_hold_speed_multiplier);
    config.general.continuous_seek_interval_ms = parse_int(
        get_value(document, "general", "continuous_seek_interval_ms"),
        config.general.continuous_seek_interval_ms);
    if (config.general.continuous_seek_interval_ms < 10) {
        config.general.continuous_seek_interval_ms = 10;
    }
    config.general.player_volume = parse_int(
        get_value(document, "general", "player_volume"),
        config.general.player_volume);
    if (config.general.player_volume < 0) {
        config.general.player_volume = 0;
    } else if (config.general.player_volume > 100) {
        config.general.player_volume = 100;
    }
    config.general.use_preferred_audio_language = parse_bool(
        get_value(document, "general", "use_preferred_audio_language"),
        config.general.use_preferred_audio_language);
    config.general.preferred_audio_language = get_value(
        document,
        "general",
        "preferred_audio_language",
        config.general.preferred_audio_language);
    config.general.use_preferred_subtitle_language = parse_bool(
        get_value(document, "general", "use_preferred_subtitle_language"),
        config.general.use_preferred_subtitle_language);
    config.general.preferred_subtitle_language = get_value(
        document,
        "general",
        "preferred_subtitle_language",
        config.general.preferred_subtitle_language);
    config.general.demux_cache_sec = parse_int(
        get_value(document, "general", "demux_cache_sec"),
        config.general.demux_cache_sec);
    config.general.resume_start_percent = parse_int(
        get_value(document, "general", "resume_start_percent"),
        config.general.resume_start_percent);
    config.general.resume_stop_percent = parse_int(
        get_value(document, "general", "resume_stop_percent"),
        config.general.resume_stop_percent);
    config.general.touch_enable = parse_bool(
        get_value(document, "general", "touch_enable"),
        config.general.touch_enable);
    config.general.touch_swipe_seek = parse_bool(
        get_value(document, "general", "touch_swipe_seek"),
        config.general.touch_swipe_seek);

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
        if (!legacySource.host.empty() || !legacySource.share.empty()) {
            config.smb_sources.push_back(std::move(legacySource));
        }
    }
}

bool write_config_file(const AppPaths& paths, const AppConfig& config) {
    std::error_code error;
    std::filesystem::create_directories(paths.base_directory, error);

    std::ofstream output(paths.config_file, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    output.write(reinterpret_cast<const char*>(utf8Bom.data()), static_cast<std::streamsize>(utf8Bom.size()));

    output << "; SwitchBOX 运行配置 / SwitchBOX runtime configuration" << '\n';
    output << "; langs/ 会相对当前 ini 所在目录查找 / langs/ is searched relative to this ini file" << '\n';
    output << '\n';

    output << "[general]" << '\n';
    output << "; 设置页可直接修改的基础设置 / Basic settings exposed in the Settings page" << '\n';
    output << "language=" << config.general.language << '\n';
    output << "; 可播放扩展名，使用逗号分隔，前导点可省略 / Comma-separated extensions, leading dots are optional." << '\n';
    output << "playable_extensions=" << config.general.playable_extensions << '\n';
    output << "; 是否显示隐藏文件 / Whether hidden files are shown" << '\n';
    output << "show_hidden=" << (config.general.show_hidden ? "true" : "false") << '\n';
    output << "; 排序方式，可选值：name_asc,name_desc,date_asc,date_desc,size_asc,size_desc / Supported values: name_asc,name_desc,date_asc,date_desc,size_asc,size_desc" << '\n';
    output << "sort_order=" << config.general.sort_order << '\n';
    output << "; 是否启用硬件解码 / Whether hardware decoding is enabled" << '\n';
    output << "hardware_decode=" << (config.general.hardware_decode ? "true" : "false") << '\n';
    output << "; 短按快进/快退秒数 / Short seek step in seconds" << '\n';
    output << "short_seek=" << config.general.short_seek << '\n';
    output << "; 长按快进/快退秒数 / Long seek step in seconds" << '\n';
    output << "long_seek=" << config.general.long_seek << '\n';
    output << "; 长按 Y 时使用的倍速 / Playback speed used while holding Y in the future player shell" << '\n';
    output << "y_hold_speed_multiplier=" << config.general.y_hold_speed_multiplier << '\n';
    output << "; 连续跳转间隔（毫秒） / Continuous seek interval in milliseconds" << '\n';
    output << "continuous_seek_interval_ms=" << config.general.continuous_seek_interval_ms << '\n';
    output << "; 是否启用音轨语言优先选择 / Whether preferred audio language is enabled" << '\n';
    output << "use_preferred_audio_language="
           << (config.general.use_preferred_audio_language ? "true" : "false") << '\n';
    output << "; 音轨语言代码，建议使用 ISO 639 风格，例如 eng、jpn、chi / Preferred audio language code, ISO 639 style recommended, for example: eng, jpn, chi" << '\n';
    output << "preferred_audio_language=" << config.general.preferred_audio_language << '\n';
    output << "; 是否启用字幕语言优先选择 / Whether preferred subtitle language is enabled" << '\n';
    output << "use_preferred_subtitle_language="
           << (config.general.use_preferred_subtitle_language ? "true" : "false") << '\n';
    output << "; 字幕语言代码，建议使用 ISO 639 风格 / Preferred subtitle language code, ISO 639 style recommended" << '\n';
    output << "preferred_subtitle_language=" << config.general.preferred_subtitle_language << '\n';
    output << '\n';
    output << "; 以下为当前仅建议在 ini 中修改的高级设置 / Advanced settings that are currently intended for ini editing only" << '\n';
    output << "; 网络播放缓存秒数，默认值参考 nxmp / Network playback cache in seconds, default inspired by nxmp" << '\n';
    output << "demux_cache_sec=" << config.general.demux_cache_sec << '\n';
    output << "; 播放进度达到该百分比后开始记录断点 / Start writing resume records after this playback progress percent" << '\n';
    output << "resume_start_percent=" << config.general.resume_start_percent << '\n';
    output << "; 剩余进度低于该百分比时不再记录断点 / Stop writing resume records when remaining progress is below this percent" << '\n';
    output << "resume_stop_percent=" << config.general.resume_stop_percent << '\n';
    output << "; 是否启用触摸操作，默认关闭 / Whether touch controls are enabled, disabled by default" << '\n';
    output << "touch_enable=" << (config.general.touch_enable ? "true" : "false") << '\n';
    output << "; 是否允许触摸滑动快进，默认关闭 / Whether touch swipe seek is enabled, disabled by default" << '\n';
    output << "touch_swipe_seek=" << (config.general.touch_swipe_seek ? "true" : "false") << '\n';
    output << "; 播放器默认音量（0-100），进入播放器时读取，退出时写回 / Player volume (0-100), loaded on player open and written back on exit" << '\n';
    output << "player_volume=" << config.general.player_volume << '\n';
    output << '\n';

    output << "; IPTV 源使用 [iptv-xxx] 形式的分组名 / IPTV sources use sections named [iptv-xxx]" << '\n';
    output << "; 示例 / Example:" << '\n';
    output << "; [iptv-main]" << '\n';
    output << "; title=主 IPTV / Main IPTV" << '\n';
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

    output << "; SMB 源使用 [smb-xxx] 形式的分组名 / SMB sources use sections named [smb-xxx]" << '\n';
    output << "; 示例 / Example:" << '\n';
    output << "; [smb-media]" << '\n';
    output << "; title=家庭 NAS / Home NAS" << '\n';
    output << "; host=192.168.1.10" << '\n';
    output << "; share=video" << '\n';
    output << "; username=user" << '\n';
    output << "; password=pass" << '\n';
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

    if (path_exists(store.paths.config_file)) {
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
