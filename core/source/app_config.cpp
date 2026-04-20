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
#include <unordered_set>

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

std::vector<std::string> split_comma_separated_list(std::string_view value) {
    std::vector<std::string> items;
    std::unordered_set<std::string> seen_items;
    std::string current;

    const auto flush = [&]() {
        std::string item = trim(current);
        current.clear();

        if (item.empty() || seen_items.contains(item)) {
            return;
        }

        seen_items.insert(item);
        items.push_back(std::move(item));
    };

    for (const char character : value) {
        if (character == ',') {
            flush();
            continue;
        }

        current.push_back(character);
    }

    flush();
    return items;
}

std::string join_comma_separated_list(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        const std::string trimmed_value = trim(value);
        if (trimmed_value.empty()) {
            continue;
        }

        if (!joined.empty()) {
            joined.push_back(',');
        }

        joined += trimmed_value;
    }

    return joined;
}

AppPaths make_paths(const std::filesystem::path& baseDirectory) {
    return {
        .base_directory = baseDirectory,
        .config_file = baseDirectory / "switchbox.ini",
        .playback_history_file = baseDirectory / ".SwitchBOX-Data" / "playback_history.json",
        .languages_directory = baseDirectory / ".SwitchBOX-Data" / "Langs",
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

std::string normalize_line_endings_to_crlf(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size() + text.size() / 8 + 8);

    for (size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            normalized += "\r\n";
            continue;
        }

        if (ch == '\n') {
            normalized += "\r\n";
            continue;
        }

        normalized.push_back(ch);
    }

    return normalized;
}

constexpr int kIniOnlyButtonLongPressThresholdDefaultMs = 260;

int read_button_long_press_threshold_ms_from_file(const std::filesystem::path& path) {
    const IniDocument document = parse_ini(path);
    const int value = parse_int(
        get_value(document, "general", "button_long_press_threshold_ms"),
        kIniOnlyButtonLongPressThresholdDefaultMs);
    return std::max(10, value);
}

const std::array<std::string_view, 22>& required_general_keys() {
    static constexpr std::array<std::string_view, 22> keys = {
        "language",
        "playable_extensions",
        "sort_order",
        "exit_to_home_screen",
        "hardware_decode",
        "short_seek",
        "long_seek",
        "y_hold_speed_multiplier",
        "continuous_seek_interval_ms",
        "player_volume",
        "player_volume_osd_duration_ms",
        "player_loading_overlay_delay_ms",
        "overlay_marquee_delay_ms",
        "use_preferred_audio_language",
        "preferred_audio_language",
        "use_preferred_subtitle_language",
        "preferred_subtitle_language",
        "demux_cache_sec",
        "resume_start_percent",
        "resume_stop_percent",
        "touch_enable",
        "touch_player_gestures",
    };
    return keys;
}

bool has_required_general_keys(const IniDocument& document) {
    const auto section_it = document.find("general");
    if (section_it == document.end()) {
        return false;
    }

    const auto& section = section_it->second;
    for (const auto key : required_general_keys()) {
        if (!section.contains(std::string(key))) {
            return false;
        }
    }

    return true;
}

bool has_required_general_keys_in_raw_text(std::string_view raw_ini_text) {
    if (raw_ini_text.empty()) {
        return false;
    }

    bool in_general_section = false;
    std::unordered_set<std::string> found_keys;
    std::istringstream input{std::string(raw_ini_text)};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with(';') || line.starts_with('#')) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            in_general_section = trim(line.substr(1, line.size() - 2)) == "general";
            continue;
        }

        if (!in_general_section) {
            continue;
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, separator));
        if (!key.empty()) {
            found_keys.insert(key);
        }
    }

    for (const auto key : required_general_keys()) {
        if (!found_keys.contains(std::string(key))) {
            return false;
        }
    }

    return true;
}

std::string general_key_value_from_config(const GeneralSettings& general, std::string_view key) {
    if (key == "language") {
        return general.language;
    }
    if (key == "playable_extensions") {
        return general.playable_extensions;
    }
    if (key == "sort_order") {
        return general.sort_order;
    }
    if (key == "exit_to_home_screen") {
        return general.exit_to_home_screen ? "true" : "false";
    }
    if (key == "hardware_decode") {
        return general.hardware_decode ? "true" : "false";
    }
    if (key == "short_seek") {
        return std::to_string(general.short_seek);
    }
    if (key == "long_seek") {
        return std::to_string(general.long_seek);
    }
    if (key == "y_hold_speed_multiplier") {
        std::ostringstream value;
        value << general.y_hold_speed_multiplier;
        return value.str();
    }
    if (key == "continuous_seek_interval_ms") {
        return std::to_string(general.continuous_seek_interval_ms);
    }
    if (key == "player_volume") {
        return std::to_string(general.player_volume);
    }
    if (key == "player_volume_osd_duration_ms") {
        return std::to_string(general.player_volume_osd_duration_ms);
    }
    if (key == "player_loading_overlay_delay_ms") {
        return std::to_string(general.player_loading_overlay_delay_ms);
    }
    if (key == "overlay_marquee_delay_ms") {
        return std::to_string(general.overlay_marquee_delay_ms);
    }
    if (key == "use_preferred_audio_language") {
        return general.use_preferred_audio_language ? "true" : "false";
    }
    if (key == "preferred_audio_language") {
        return general.preferred_audio_language;
    }
    if (key == "use_preferred_subtitle_language") {
        return general.use_preferred_subtitle_language ? "true" : "false";
    }
    if (key == "preferred_subtitle_language") {
        return general.preferred_subtitle_language;
    }
    if (key == "demux_cache_sec") {
        return std::to_string(general.demux_cache_sec);
    }
    if (key == "resume_start_percent") {
        return std::to_string(general.resume_start_percent);
    }
    if (key == "resume_stop_percent") {
        return std::to_string(general.resume_stop_percent);
    }
    if (key == "touch_enable") {
        return general.touch_enable ? "true" : "false";
    }
    if (key == "touch_player_gestures") {
        return general.touch_player_gestures ? "true" : "false";
    }
    return {};
}

std::vector<std::string> general_key_comment_lines(std::string_view key) {
    if (key == "exit_to_home_screen") {
        return {"; Exit to home screen on app quit / Whether app exit returns to the home screen"};
    }
    if (key == "language") {
        return {"; 璁剧疆椤靛彲鐩存帴淇敼鐨勫熀纭€璁剧疆 / Basic settings exposed in the Settings page"};
    }
    if (key == "playable_extensions") {
        return {"; 鍙挱鏀炬墿灞曞悕锛屼娇鐢ㄩ€楀彿鍒嗛殧锛屽墠瀵肩偣鍙渷鐣?/ Comma-separated extensions, leading dots are optional."};
    }
    if (key == "sort_order") {
        return {"; 鎺掑簭鏂瑰紡锛屽彲閫夊€硷細name_asc,name_desc,date_asc,date_desc,size_asc,size_desc / Supported values: name_asc,name_desc,date_asc,date_desc,size_asc,size_desc"};
    }
    if (key == "hardware_decode") {
        return {
            "; ----涓嬮潰鏄挱鏀惧櫒璁剧疆---- / Player settings",
            "; 鏄惁鍚敤纭欢瑙ｇ爜 / Whether hardware decoding is enabled",
        };
    }
    if (key == "short_seek") {
        return {"; 鐭寜蹇繘/蹇€€绉掓暟 / Short seek step in seconds"};
    }
    if (key == "long_seek") {
        return {"; 闀挎寜蹇繘/蹇€€绉掓暟 / Long seek step in seconds"};
    }
    if (key == "y_hold_speed_multiplier") {
        return {"; Y 鍊嶉€熸ā寮忎娇鐢ㄧ殑鍊嶉€燂紱鐐规寜 Y 寮€鍏筹紝闀挎寜 Y 浼氬啀鍙犲姞涓€娆″悓鍊煎€嶉€?/ Playback speed used for Y speed mode; tap Y toggles it, and holding Y adds the same speed once more"};
    }
    if (key == "continuous_seek_interval_ms") {
        return {"; 杩炵画璺宠浆闂撮殧锛堟绉掞級 / Continuous seek interval in milliseconds"};
    }
    if (key == "use_preferred_audio_language") {
        return {"; 鏄惁鍚敤闊宠建璇█浼樺厛閫夋嫨 / Whether preferred audio language is enabled"};
    }
    if (key == "preferred_audio_language") {
        return {"; 闊宠建璇█浠ｇ爜锛泎h=涓枃浼樺厛锛堝箍娉涘尮閰嶅苟浼樺厛鏅€氳瘽锛夛紝en=鑻辨枃浼樺厛锛屼篃鍙嚜瀹氫箟杈撳叆濡?eng銆乯pn銆乧hi / Preferred audio language code; zh=prefer Chinese broadly with Mandarin priority, en=prefer English, or enter custom text such as eng, jpn, chi"};
    }
    if (key == "use_preferred_subtitle_language") {
        return {"; 鏄惁鍚敤瀛楀箷璇█浼樺厛閫夋嫨 / Whether preferred subtitle language is enabled"};
    }
    if (key == "preferred_subtitle_language") {
        return {"; 瀛楀箷璇█浠ｇ爜锛泎h=涓枃浼樺厛锛堝箍娉涘尮閰嶅苟浼樺厛绠€浣撲腑鏂囷級锛宔n=鑻辨枃浼樺厛锛屼篃鍙嚜瀹氫箟杈撳叆濡?eng銆乯pn銆乧hi / Preferred subtitle language code; zh=prefer Chinese broadly with Simplified Chinese priority, en=prefer English, or enter custom text such as eng, jpn, chi"};
    }
    if (key == "demux_cache_sec") {
        return {
            "; -----浠ヤ笅涓哄綋鍓嶄粎鍙湪 ini 涓慨鏀圭殑楂樼骇璁剧疆---- / Advanced settings that are currently intended for ini editing only",
            "; 缃戠粶鎾斁缂撳瓨绉掓暟锛岄粯璁ゅ€煎弬鑰?nxmp / Network playback cache in seconds, default inspired by nxmp",
        };
    }
    if (key == "resume_start_percent") {
        return {"; 鎾斁杩涘害杈惧埌璇ョ櫨鍒嗘瘮鍚庡紑濮嬭褰曟柇鐐?/ Start writing resume records after this playback progress percent"};
    }
    if (key == "resume_stop_percent") {
        return {"; 鍓╀綑杩涘害浣庝簬璇ョ櫨鍒嗘瘮鏃朵笉鍐嶈褰曟柇鐐?/ Stop writing resume records when remaining progress is below this percent"};
    }
    if (key == "touch_enable") {
        return {"; 鏄惁鍚敤瑙︽懜鎿嶄綔 / Whether touch controls are enabled"};
    }
    if (key == "touch_player_gestures") {
        return {
            "; 鏄惁鍚敤鎾斁鍣ㄨЕ鎺ф墜鍔匡紙鍙屽嚮鏆傚仠銆佸乏鍙虫粦鍔ㄨ烦杞€佷笂涓嬫粦鍔ㄩ煶閲忋€佺偣鍑昏繘搴︽潯瀹氫綅锛?/ Whether player touch gestures are enabled (double-tap pause, horizontal seek, vertical volume, progress-bar tap seek)"};
    }
    if (key == "player_volume") {
        return {"; 鎾斁鍣ㄩ粯璁ら煶閲忥紙0-100锛夛紝杩涘叆鎾斁鍣ㄦ椂璇诲彇锛岄€€鍑烘椂鍐欏洖 / Player volume (0-100), loaded on player open and written back on exit"};
    }
    if (key == "player_volume_osd_duration_ms") {
        return {"; 鍙充晶闊抽噺娴獥鏄剧ず鏃堕暱锛堟绉掞級锛?=涓嶆樉绀?/ Right-side volume OSD duration in milliseconds, 0 = disabled"};
    }
    if (key == "overlay_marquee_delay_ms") {
        return {"; 宸︿晶娴獥鐒︾偣鍋滅暀鍚庡紑濮嬫粴鍔ㄧ殑寤惰繜锛堟绉掞級锛?=绔嬪嵆婊氬姩 / Delay before marquee starts on focused item in left overlay (milliseconds), 0 = immediate"};
    }

    return {};
}

#if 0
bool backfill_missing_general_keys_in_file_legacy(const AppPaths& paths, const AppConfig& config) {
    const std::string raw_ini_text = read_utf8_text_file(paths.config_file);
    if (raw_ini_text.empty()) {
        return false;
    }

    std::vector<std::string> lines;
    lines.reserve(256);
    {
        std::istringstream input{raw_ini_text};
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }

    int general_start = -1;
    int general_end = static_cast<int>(lines.size());
    for (int index = 0; index < static_cast<int>(lines.size()); ++index) {
        const std::string trimmed = trim(lines[static_cast<size_t>(index)]);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string section_name = trim(trimmed.substr(1, trimmed.size() - 2));
            if (general_start < 0) {
                if (section_name == "general") {
                    general_start = index;
                }
                continue;
            }

            general_end = index;
            break;
        }
    }

    if (general_start < 0) {
        return false;
    }

    std::unordered_set<std::string> existing_keys;
    for (int index = general_start + 1; index < general_end; ++index) {
        const std::string trimmed = trim(lines[static_cast<size_t>(index)]);
        if (trimmed.empty() || trimmed.starts_with(';') || trimmed.starts_with('#')) {
            continue;
        }
        const size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(trimmed.substr(0, separator));
        if (!key.empty()) {
            existing_keys.insert(key);
        }
    }

    std::vector<std::string> missing_lines;
    for (const auto key : required_general_keys()) {
        if (existing_keys.contains(std::string(key))) {
            continue;
        }
        for (const auto& comment_line : general_key_comment_lines(key)) {
            missing_lines.push_back(comment_line);
        }
        if (key == std::string_view("player_loading_overlay_delay_ms")) {
            missing_lines.push_back(
                "; IPTV 鎾斁绛夊緟鎻愮ず鍑虹幇鍓嶇殑寤惰繜锛堟绉掞級锛?=绔嬪嵆鏄剧ず / Delay before showing the IPTV loading overlay while waiting for playback (milliseconds), 0 = immediate");
        }
        missing_lines.push_back(std::string(key) + "=" + general_key_value_from_config(config.general, key));
    }

    if (missing_lines.empty()) {
        return true;
    }

    std::vector<std::string> insert_lines;
    insert_lines.reserve(missing_lines.size() + 2);
    insert_lines.push_back("; Auto-backfilled missing [general] keys / Auto inserted missing [general] settings");
    for (const auto& line : missing_lines) {
        insert_lines.push_back(line);
    }

    lines.insert(lines.begin() + general_end, insert_lines.begin(), insert_lines.end());

    std::ofstream output(paths.config_file, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    output.write(reinterpret_cast<const char*>(utf8Bom.data()), static_cast<std::streamsize>(utf8Bom.size()));
    for (size_t index = 0; index < lines.size(); ++index) {
        output << lines[index];
        if (index + 1 < lines.size()) {
            output << '\n';
        }
    }
    std::vector<std::string> rendered_lines;
    {
        std::istringstream input(output.str());
        std::string line;
        while (std::getline(input, line)) {
            if (line.find("Left overlay marquee delay") != std::string::npos) {
                line = "; Left overlay marquee delay in milliseconds, 0 = immediate";
            }
            rendered_lines.push_back(std::move(line));
        }
    }

    std::ostringstream rendered;
    for (size_t index = 0; index < rendered_lines.size(); ++index) {
        rendered << rendered_lines[index];
        if (index + 1 < rendered_lines.size()) {
            rendered << '\n';
        }
    }

    std::ofstream file(paths.config_file, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    file.write(reinterpret_cast<const char*>(utf8Bom.data()), static_cast<std::streamsize>(utf8Bom.size()));

    const std::string normalized = normalize_line_endings_to_crlf(rendered.str());
    file.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
    return file.good();
}

}
#endif

bool backfill_missing_general_keys_in_file(const AppPaths& paths, const AppConfig& config) {
    const std::string raw_ini_text = read_utf8_text_file(paths.config_file);
    if (raw_ini_text.empty()) {
        return false;
    }

    std::vector<std::string> lines;
    lines.reserve(256);
    {
        std::istringstream input{raw_ini_text};
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }

    int general_start = -1;
    int general_end = static_cast<int>(lines.size());
    for (int index = 0; index < static_cast<int>(lines.size()); ++index) {
        const std::string trimmed = trim(lines[static_cast<size_t>(index)]);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string section_name = trim(trimmed.substr(1, trimmed.size() - 2));
            if (general_start < 0) {
                if (section_name == "general") {
                    general_start = index;
                }
                continue;
            }

            general_end = index;
            break;
        }
    }

    if (general_start < 0) {
        return false;
    }

    std::unordered_set<std::string> existing_keys;
    for (int index = general_start + 1; index < general_end; ++index) {
        const std::string trimmed = trim(lines[static_cast<size_t>(index)]);
        if (trimmed.empty() || trimmed.starts_with(';') || trimmed.starts_with('#')) {
            continue;
        }
        const size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(trimmed.substr(0, separator));
        if (!key.empty()) {
            existing_keys.insert(key);
        }
    }

    std::vector<std::string> missing_lines;
    for (const auto key : required_general_keys()) {
        if (existing_keys.contains(std::string(key))) {
            continue;
        }
        for (const auto& comment_line : general_key_comment_lines(key)) {
            missing_lines.push_back(comment_line);
        }
        if (key == std::string_view("player_loading_overlay_delay_ms")) {
            missing_lines.push_back(
                "; IPTV 鎾斁绛夊緟鎻愮ず鍑虹幇鍓嶇殑寤惰繜锛堟绉掞級锛?=绔嬪嵆鏄剧ず / Delay before showing the IPTV loading overlay while waiting for playback (milliseconds), 0 = immediate");
        }
        missing_lines.push_back(std::string(key) + "=" + general_key_value_from_config(config.general, key));
    }

    if (missing_lines.empty()) {
        return true;
    }

    std::vector<std::string> insert_lines;
    insert_lines.reserve(missing_lines.size() + 1);
    insert_lines.push_back("; Auto-backfilled missing [general] keys / Auto inserted missing [general] settings");
    for (const auto& line : missing_lines) {
        insert_lines.push_back(line);
    }

    lines.insert(lines.begin() + general_end, insert_lines.begin(), insert_lines.end());

    std::ofstream output(paths.config_file, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    output.write(reinterpret_cast<const char*>(utf8Bom.data()), static_cast<std::streamsize>(utf8Bom.size()));

    std::ostringstream rendered;
    for (size_t index = 0; index < lines.size(); ++index) {
        rendered << lines[index];
        if (index + 1 < lines.size()) {
            rendered << '\n';
        }
    }

    const std::string normalized = normalize_line_endings_to_crlf(rendered.str());
    output.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
    return output.good();
}

void load_config_from_document(const IniDocument& document, AppConfig& config) {
    config.general.language =
        get_value(document, "general", "language", config.general.language);
    config.general.playable_extensions =
        get_value(document, "general", "playable_extensions", config.general.playable_extensions);
    config.general.sort_order =
        get_value(document, "general", "sort_order", config.general.sort_order);
    config.general.exit_to_home_screen = parse_bool(
        get_value(document, "general", "exit_to_home_screen"),
        config.general.exit_to_home_screen);
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
    config.general.player_volume_osd_duration_ms = parse_int(
        get_value(document, "general", "player_volume_osd_duration_ms"),
        config.general.player_volume_osd_duration_ms);
    if (config.general.player_volume_osd_duration_ms < 0) {
        config.general.player_volume_osd_duration_ms = 0;
    }
    config.general.player_loading_overlay_delay_ms = parse_int(
        get_value(document, "general", "player_loading_overlay_delay_ms"),
        config.general.player_loading_overlay_delay_ms);
    if (config.general.player_loading_overlay_delay_ms < 0) {
        config.general.player_loading_overlay_delay_ms = 0;
    }
    config.general.overlay_marquee_delay_ms = parse_int(
        get_value(document, "general", "overlay_marquee_delay_ms"),
        config.general.overlay_marquee_delay_ms);
    if (config.general.overlay_marquee_delay_ms < 0) {
        config.general.overlay_marquee_delay_ms = 0;
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
    config.general.touch_player_gestures = parse_bool(
        get_value(
            document,
            "general",
            "touch_player_gestures",
            get_value(document, "general", "touch_swipe_seek")),
        config.general.touch_player_gestures);

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
            source.use_history = parse_bool(
                get_value(document, sectionName, "use_history"),
                source.use_history);
            source.favorite_keys = split_comma_separated_list(
                get_value(document, sectionName, "favorite_keys"));
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
            source.use_history = parse_bool(
                get_value(document, sectionName, "use_history"),
                source.use_history);
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
        legacySource.use_history = parse_bool(
            get_value(document, "iptv", "use_history"),
            legacySource.use_history);
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
        legacySource.use_history = parse_bool(
            get_value(document, "smb", "use_history"),
            legacySource.use_history);
        if (!legacySource.host.empty() || !legacySource.share.empty()) {
            config.smb_sources.push_back(std::move(legacySource));
        }
    }
}

bool write_config_file(const AppPaths& paths, const AppConfig& config) {
    std::error_code error;
    std::filesystem::create_directories(paths.base_directory, error);

    std::ostringstream output;
    output << "; SwitchBOX 杩愯閰嶇疆 / SwitchBOX runtime configuration" << '\n';
    output << "; .SwitchBOX-Data/Langs/ 浼氱浉瀵瑰綋鍓?ini 鎵€鍦ㄧ洰褰曟煡鎵?/ .SwitchBOX-Data/Langs/ is searched relative to this ini file" << '\n';
    output << '\n';
    output << "; -----------杞欢鍙傛暟--------------" << '\n';
    output << "[general]" << '\n';
    output << "; 璁剧疆椤靛彲鐩存帴淇敼鐨勫熀纭€璁剧疆 / Basic settings exposed in the Settings page" << '\n';
    output << "language=" << config.general.language << '\n';
    output << '\n';
    output << "; 鍙挱鏀炬墿灞曞悕锛屼娇鐢ㄩ€楀彿鍒嗛殧锛屽墠瀵肩偣鍙渷鐣?/ Comma-separated extensions, leading dots are optional." << '\n';
    output << "playable_extensions=" << config.general.playable_extensions << '\n';
    output << '\n';
    output << "; 鎺掑簭鏂瑰紡锛屽彲閫夊€硷細name_asc,name_desc,date_asc,date_desc,size_asc,size_desc / Supported values: name_asc,name_desc,date_asc,date_desc,size_asc,size_desc" << '\n';
    output << "sort_order=" << config.general.sort_order << '\n';
    output << '\n';
    output << "; Exit to home screen on app quit / Whether app exit returns to the home screen" << '\n';
    output << "exit_to_home_screen=" << (config.general.exit_to_home_screen ? "true" : "false") << '\n';
    output << '\n';
    output << "; ----涓嬮潰鏄挱鏀惧櫒璁剧疆---- / Player settings" << '\n';
    output << "; 鏄惁鍚敤纭欢瑙ｇ爜 / Whether hardware decoding is enabled" << '\n';
    output << "hardware_decode=" << (config.general.hardware_decode ? "true" : "false") << '\n';
    output << '\n';
    output << "; 鐭寜蹇繘/蹇€€绉掓暟 / Short seek step in seconds" << '\n';
    output << "short_seek=" << config.general.short_seek << '\n';
    output << '\n';
    output << "; 闀挎寜蹇繘/蹇€€绉掓暟 / Long seek step in seconds" << '\n';
    output << "long_seek=" << config.general.long_seek << '\n';
    output << '\n';
    output << "; Y 鍊嶉€熸ā寮忎娇鐢ㄧ殑鍊嶉€燂紱鐐规寜 Y 寮€鍏筹紝闀挎寜 Y 浼氬啀鍙犲姞涓€娆″悓鍊煎€嶉€?/ Playback speed used for Y speed mode; tap Y toggles it, and holding Y adds the same speed once more" << '\n';
    output << "y_hold_speed_multiplier=" << config.general.y_hold_speed_multiplier << '\n';
    output << '\n';
    output << "; 杩炵画璺宠浆闂撮殧锛堟绉掞級 / Continuous seek interval in milliseconds" << '\n';
    output << "continuous_seek_interval_ms=" << config.general.continuous_seek_interval_ms << '\n';
    output << '\n';
    output << "; 鏄惁鍚敤闊宠建璇█浼樺厛閫夋嫨 / Whether preferred audio language is enabled" << '\n';
    output << "use_preferred_audio_language=" << (config.general.use_preferred_audio_language ? "true" : "false") << '\n';
    output << '\n';
    output << "; 闊宠建璇█浠ｇ爜锛泎h=涓枃浼樺厛锛堝箍娉涘尮閰嶅苟浼樺厛鏅€氳瘽锛夛紝en=鑻辨枃浼樺厛锛屼篃鍙嚜瀹氫箟杈撳叆濡?eng銆乯pn銆乧hi / Preferred audio language code; zh=prefer Chinese broadly with Mandarin priority, en=prefer English, or enter custom text such as eng, jpn, chi" << '\n';
    output << "preferred_audio_language=" << config.general.preferred_audio_language << '\n';
    output << '\n';
    output << "; 鏄惁鍚敤瀛楀箷璇█浼樺厛閫夋嫨 / Whether preferred subtitle language is enabled" << '\n';
    output << "use_preferred_subtitle_language=" << (config.general.use_preferred_subtitle_language ? "true" : "false") << '\n';
    output << '\n';
    output << "; 瀛楀箷璇█浠ｇ爜锛泎h=涓枃浼樺厛锛堝箍娉涘尮閰嶅苟浼樺厛绠€浣撲腑鏂囷級锛宔n=鑻辨枃浼樺厛锛屼篃鍙嚜瀹氫箟杈撳叆濡?eng銆乯pn銆乧hi / Preferred subtitle language code; zh=prefer Chinese broadly with Simplified Chinese priority, en=prefer English, or enter custom text such as eng, jpn, chi" << '\n';
    output << "preferred_subtitle_language=" << config.general.preferred_subtitle_language << '\n';
    output << '\n';
    output << "; -----浠ヤ笅涓哄綋鍓嶄粎鍙湪 ini 涓慨鏀圭殑楂樼骇璁剧疆---- / Advanced settings that are currently intended for ini editing only" << '\n';
    output << "; 杞欢鍐呯偣鎸?闀挎寜鍒ゅ畾闃堝€硷紙姣锛?/ Global software tap-vs-hold threshold in milliseconds" << '\n';
    output << "button_long_press_threshold_ms=" << read_button_long_press_threshold_ms_from_file(paths.config_file) << '\n';
    output << '\n';
    output << "; 缃戠粶鎾斁缂撳瓨绉掓暟锛岄粯璁ゅ€煎弬鑰?nxmp / Network playback cache in seconds, default inspired by nxmp" << '\n';
    output << "demux_cache_sec=" << config.general.demux_cache_sec << '\n';
    output << '\n';
    output << "; 鎾斁杩涘害杈惧埌璇ョ櫨鍒嗘瘮鍚庡紑濮嬭褰曟柇鐐?/ Start writing resume records after this playback progress percent" << '\n';
    output << "resume_start_percent=" << config.general.resume_start_percent << '\n';
    output << '\n';
    output << "; 鍓╀綑杩涘害浣庝簬璇ョ櫨鍒嗘瘮鏃朵笉鍐嶈褰曟柇鐐?/ Stop writing resume records when remaining progress is below this percent" << '\n';
    output << "resume_stop_percent=" << config.general.resume_stop_percent << '\n';
    output << '\n';
    output << "; 鏄惁鍚敤瑙︽懜鎿嶄綔 / Whether touch controls are enabled" << '\n';
    output << "touch_enable=" << (config.general.touch_enable ? "true" : "false") << '\n';
    output << '\n';
    output << "; 鏄惁鍚敤鎾斁鍣ㄨЕ鎺ф墜鍔匡紙鍙屽嚮鏆傚仠銆佸乏鍙虫粦鍔ㄨ烦杞€佷笂涓嬫粦鍔ㄩ煶閲忋€佺偣鍑昏繘搴︽潯瀹氫綅锛?/ Whether player touch gestures are enabled (double-tap pause, horizontal seek, vertical volume, progress-bar tap seek)" << '\n';
    output << "touch_player_gestures=" << (config.general.touch_player_gestures ? "true" : "false") << '\n';
    output << '\n';
    output << "; 鎾斁鍣ㄩ粯璁ら煶閲忥紙0-100锛夛紝杩涘叆鎾斁鍣ㄦ椂璇诲彇锛岄€€鍑烘椂鍐欏洖 / Player volume (0-100), loaded on player open and written back on exit" << '\n';
    output << "player_volume=" << config.general.player_volume << '\n';
    output << '\n';
    output << "; 鍙充晶闊抽噺娴獥鏄剧ず鏃堕暱锛堟绉掞級锛?=涓嶆樉绀?/ Right-side volume OSD duration in milliseconds, 0 = disabled" << '\n';
    output << "player_volume_osd_duration_ms=" << config.general.player_volume_osd_duration_ms << '\n';
    output << '\n';
    output << "; 宸︿晶娴獥鐒︾偣鍋滅暀鍚庡紑濮嬫粴鍔ㄧ殑寤惰繜锛堟绉掞級锛?=绔嬪嵆婊氬姩 / Delay before marquee starts on focused item in left overlay (milliseconds), 0 = immediate" << '\n';
    output << "; IPTV 鎾斁绛夊緟鎻愮ず鍑虹幇鍓嶇殑寤惰繜锛堟绉掞級锛?=绔嬪嵆鏄剧ず / Delay before showing the IPTV loading overlay while waiting for playback (milliseconds), 0 = immediate" << '\n';
    output << "player_loading_overlay_delay_ms=" << config.general.player_loading_overlay_delay_ms << '\n';
    output << '\n';
    output << "; 宸︿晶娴獥鐒︾偣鍋滅暀鍚庡紑濮嬫粴鍔ㄧ殑寤惰繜锛堟绉掞級锛?=绔嬪嵆婊氬姩 / Delay before marquee starts on focused item in left overlay (milliseconds), 0 = immediate" << '\n';
    output << "overlay_marquee_delay_ms=" << config.general.overlay_marquee_delay_ms << '\n';
    output << '\n';
    output << "; -----------IPTV--------------" << '\n';
    output << "; IPTV 婧愪娇鐢?[iptv-xxx] 褰㈠紡鐨勫垎缁勫悕 / IPTV sources use sections named [iptv-xxx]" << '\n';
    output << "; 绀轰緥 / Example:" << '\n';
    output << "; [iptv-main]" << '\n';
    output << "; title=涓?IPTV / Main IPTV" << '\n';
    output << "; url=http://example.com/playlist.m3u" << '\n';
    output << "; enabled=true" << '\n';
    output << "; use_history=true" << '\n';
    output << "; favorite_keys=channel1,channel2" << '\n';
    output << '\n';

    for (const auto& source : config.iptv_sources) {
        if (source.key.empty()) {
            continue;
        }

        output << "[iptv-" << source.key << "]" << '\n';
        output << "title=" << source.title << '\n';
        output << "url=" << source.url << '\n';
        output << "enabled=" << (source.enabled ? "true" : "false") << '\n';
        output << "use_history=" << (source.use_history ? "true" : "false") << '\n';
        output << "favorite_keys=" << join_comma_separated_list(source.favorite_keys) << '\n';
        output << '\n';
    }

    output << "; -----------SMB--------------" << '\n';
    output << "; SMB 婧愪娇鐢?[smb-xxx] 褰㈠紡鐨勫垎缁勫悕 / SMB sources use sections named [smb-xxx]" << '\n';
    output << "; 绀轰緥 / Example:" << '\n';
    output << "; [smb-media]" << '\n';
    output << "; title=瀹跺涵 NAS / Home NAS" << '\n';
    output << "; host=192.168.1.10" << '\n';
    output << "; share=video" << '\n';
    output << "; username=user" << '\n';
    output << "; password=pass" << '\n';
    output << "; enabled=true" << '\n';
    output << "; use_history=true" << '\n';
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
        output << "use_history=" << (source.use_history ? "true" : "false") << '\n';
        output << '\n';
    }

    std::ofstream file(paths.config_file, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    file.write(reinterpret_cast<const char*>(utf8Bom.data()), static_cast<std::streamsize>(utf8Bom.size()));

    const std::string normalized = normalize_line_endings_to_crlf(output.str());
    file.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
    return file.good();
#if 0

    output << "; SwitchBOX 杩愯閰嶇疆 / SwitchBOX runtime configuration" << '\n';
    output << "; .SwitchBOX-Data/Langs/ 浼氱浉瀵瑰綋鍓?ini 鎵€鍦ㄧ洰褰曟煡鎵?/ .SwitchBOX-Data/Langs/ is searched relative to this ini file" << '\n';
    output << '\n';

    output << "[general]" << '\n';
    output << "; 璁剧疆椤靛彲鐩存帴淇敼鐨勫熀纭€璁剧疆 / Basic settings exposed in the Settings page" << '\n';
    output << "language=" << config.general.language << '\n';
    output << "; 鍙挱鏀炬墿灞曞悕锛屼娇鐢ㄩ€楀彿鍒嗛殧锛屽墠瀵肩偣鍙渷鐣?/ Comma-separated extensions, leading dots are optional." << '\n';
    output << "playable_extensions=" << config.general.playable_extensions << '\n';
    output << "; 鎺掑簭鏂瑰紡锛屽彲閫夊€硷細name_asc,name_desc,date_asc,date_desc,size_asc,size_desc / Supported values: name_asc,name_desc,date_asc,date_desc,size_asc,size_desc" << '\n';
    output << "sort_order=" << config.general.sort_order << '\n';
    output << "; 鏄惁鍚敤纭欢瑙ｇ爜 / Whether hardware decoding is enabled" << '\n';
    output << "hardware_decode=" << (config.general.hardware_decode ? "true" : "false") << '\n';
    output << "; 鐭寜蹇繘/蹇€€绉掓暟 / Short seek step in seconds" << '\n';
    output << "short_seek=" << config.general.short_seek << '\n';
    output << "; 闀挎寜蹇繘/蹇€€绉掓暟 / Long seek step in seconds" << '\n';
    output << "long_seek=" << config.general.long_seek << '\n';
    output << "; Y 鍊嶉€熸ā寮忎娇鐢ㄧ殑鍊嶉€燂紱鐐规寜 Y 寮€鍏筹紝闀挎寜 Y 浼氬啀鍙犲姞涓€娆″悓鍊煎€嶉€?/ Playback speed used for Y speed mode; tap Y toggles it, and holding Y adds the same speed once more" << '\n';
    output << "y_hold_speed_multiplier=" << config.general.y_hold_speed_multiplier << '\n';
    output << "; 杩炵画璺宠浆闂撮殧锛堟绉掞級 / Continuous seek interval in milliseconds" << '\n';
    output << "continuous_seek_interval_ms=" << config.general.continuous_seek_interval_ms << '\n';
    output << "; 鏄惁鍚敤闊宠建璇█浼樺厛閫夋嫨 / Whether preferred audio language is enabled" << '\n';
    output << "use_preferred_audio_language="
           << (config.general.use_preferred_audio_language ? "true" : "false") << '\n';
    output << "; 闊宠建璇█浠ｇ爜锛屽缓璁娇鐢?ISO 639 椋庢牸锛屼緥濡?eng銆乯pn銆乧hi / Preferred audio language code, ISO 639 style recommended, for example: eng, jpn, chi" << '\n';
    output << "preferred_audio_language=" << config.general.preferred_audio_language << '\n';
    output << "; 鏄惁鍚敤瀛楀箷璇█浼樺厛閫夋嫨 / Whether preferred subtitle language is enabled" << '\n';
    output << "use_preferred_subtitle_language="
           << (config.general.use_preferred_subtitle_language ? "true" : "false") << '\n';
    output << "; 瀛楀箷璇█浠ｇ爜锛屽缓璁娇鐢?ISO 639 椋庢牸 / Preferred subtitle language code, ISO 639 style recommended" << '\n';
    output << "preferred_subtitle_language=" << config.general.preferred_subtitle_language << '\n';
    output << '\n';
    output << "; 浠ヤ笅涓哄綋鍓嶄粎寤鸿鍦?ini 涓慨鏀圭殑楂樼骇璁剧疆 / Advanced settings that are currently intended for ini editing only" << '\n';
    output << "; 缃戠粶鎾斁缂撳瓨绉掓暟锛岄粯璁ゅ€煎弬鑰?nxmp / Network playback cache in seconds, default inspired by nxmp" << '\n';
    output << "demux_cache_sec=" << config.general.demux_cache_sec << '\n';
    output << "; 鎾斁杩涘害杈惧埌璇ョ櫨鍒嗘瘮鍚庡紑濮嬭褰曟柇鐐?/ Start writing resume records after this playback progress percent" << '\n';
    output << "resume_start_percent=" << config.general.resume_start_percent << '\n';
    output << "; 鍓╀綑杩涘害浣庝簬璇ョ櫨鍒嗘瘮鏃朵笉鍐嶈褰曟柇鐐?/ Stop writing resume records when remaining progress is below this percent" << '\n';
    output << "resume_stop_percent=" << config.general.resume_stop_percent << '\n';
    output << "; 鏄惁鍚敤瑙︽懜鎿嶄綔锛岄粯璁ゅ叧闂?/ Whether touch controls are enabled, disabled by default" << '\n';
    output << "touch_enable=" << (config.general.touch_enable ? "true" : "false") << '\n';
    output << "; 鏄惁鍚敤鎾斁鍣ㄨЕ鎺ф墜鍔匡紙鍙屽嚮鏆傚仠銆佸乏鍙虫粦鍔ㄨ烦杞€佷笂涓嬫粦鍔ㄩ煶閲忋€佺偣鍑昏繘搴︽潯瀹氫綅锛夛紝榛樿鍏抽棴 / Whether player touch gestures are enabled (double-tap pause, horizontal seek, vertical volume, progress-bar tap seek), disabled by default" << '\n';
    output << "touch_player_gestures=" << (config.general.touch_player_gestures ? "true" : "false") << '\n';
    output << "; 鎾斁鍣ㄩ粯璁ら煶閲忥紙0-100锛夛紝杩涘叆鎾斁鍣ㄦ椂璇诲彇锛岄€€鍑烘椂鍐欏洖 / Player volume (0-100), loaded on player open and written back on exit" << '\n';
    output << "player_volume=" << config.general.player_volume << '\n';
    output << "; 鍙充晶闊抽噺娴獥鏄剧ず鏃堕暱锛堟绉掞級锛?=涓嶆樉绀?/ Right-side volume OSD duration in milliseconds, 0 = disabled" << '\n';
    output << "player_volume_osd_duration_ms=" << config.general.player_volume_osd_duration_ms << '\n';
    output << "; Left overlay marquee delay in milliseconds, 0 = immediate" << '\n';
    output << "overlay_marquee_delay_ms=" << config.general.overlay_marquee_delay_ms << '\n';
    output << '\n';

    output << "; IPTV 婧愪娇鐢?[iptv-xxx] 褰㈠紡鐨勫垎缁勫悕 / IPTV sources use sections named [iptv-xxx]" << '\n';
    output << "; 绀轰緥 / Example:" << '\n';
    output << "; [iptv-main]" << '\n';
    output << "; title=涓?IPTV / Main IPTV" << '\n';
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

    output << "; SMB 婧愪娇鐢?[smb-xxx] 褰㈠紡鐨勫垎缁勫悕 / SMB sources use sections named [smb-xxx]" << '\n';
    output << "; 绀轰緥 / Example:" << '\n';
    output << "; [smb-media]" << '\n';
    output << "; title=瀹跺涵 NAS / Home NAS" << '\n';
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

    std::vector<std::string> rendered_lines;
    {
        std::istringstream input(output.str());
        std::string line;
        while (std::getline(input, line)) {
            if (line.find("Left overlay marquee delay") != std::string::npos) {
                line = "; Left overlay marquee delay in milliseconds, 0 = immediate";
            }
            rendered_lines.push_back(std::move(line));
        }
    }

    std::ostringstream rendered;
    for (size_t index = 0; index < rendered_lines.size(); ++index) {
        rendered << rendered_lines[index];
        if (index + 1 < rendered_lines.size()) {
            rendered << '\n';
        }
    }

    std::ofstream file(paths.config_file, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    static constexpr std::array<unsigned char, 3> utf8Bom = {0xEF, 0xBB, 0xBF};
    file.write(reinterpret_cast<const char*>(utf8Bom.data()), static_cast<std::streamsize>(utf8Bom.size()));

    const std::string normalized = normalize_line_endings_to_crlf(rendered.str());
    file.write(normalized.data(), static_cast<std::streamsize>(normalized.size()));
    return file.good();
#endif
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
        const std::string raw_ini_text = read_utf8_text_file(store.paths.config_file);
        load_config_from_document(document, store.config);
        store.loadedFromDisk = true;
        store.initialized = true;
        const bool needs_legacy_upgrade = document.contains("iptv") || document.contains("smb");
        const bool needs_general_backfill =
            !has_required_general_keys(document) || !has_required_general_keys_in_raw_text(raw_ini_text);
        if (needs_legacy_upgrade) {
            write_config_file(store.paths, store.config);
        } else if (needs_general_backfill) {
            backfill_missing_general_keys_in_file(store.paths, store.config);
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

int ini_only_button_long_press_threshold_ms() {
    return read_button_long_press_threshold_ms_from_file(AppConfigStore::paths().config_file);
}

}  // namespace switchbox::core
