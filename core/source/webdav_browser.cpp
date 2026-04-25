#include "switchbox/core/webdav_browser.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

#include <tinyxml2/tinyxml2.h>

#include "switchbox/core/build_info.hpp"
#include "switchbox/core/smb_browser.hpp"

#if __has_include(<switch.h>) && __has_include(<netdb.h>) && __has_include(<sys/socket.h>) && __has_include(<sys/time.h>) && __has_include(<unistd.h>)
#define SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT 1
#include <switch.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#define SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT 0
#endif

#if SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT
extern "C" struct hostent* gethostbyname(const char* name);
#endif

namespace switchbox::core {

namespace {

constexpr bool kWebDavDebugLogEnabled = false;

struct WebDavDebugLogState {
    std::mutex mutex;
    bool initialized = false;
    uint64_t sequence = 0;
    uint64_t session_token = 0;
    std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::time_point::min();
};

WebDavDebugLogState& webdav_debug_log_state() {
    static WebDavDebugLogState state;
    return state;
}

std::filesystem::path webdav_debug_log_path() {
    return switchbox::core::AppConfigStore::paths().base_directory / "webdav_debug.log";
}

uint64_t make_webdav_debug_session_token() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto token = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return token == 0 ? 1 : token;
}

std::string sanitize_debug_log_message(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    return value;
}

std::string summarize_body_for_debug_log(std::string value) {
    value = sanitize_debug_log_message(std::move(value));
    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char character) {
            return (character < 32 && character != ' ') || character == 127;
        }),
        value.end());

    constexpr size_t kMaxLength = 240;
    if (value.size() <= kMaxLength) {
        return value;
    }

    return value.substr(0, kMaxLength) + "...(len=" + std::to_string(value.size()) + ")";
}

std::string sanitize_url_for_debug_log(const std::string& raw_url) {
    const size_t scheme_separator = raw_url.find("://");
    if (scheme_separator == std::string::npos) {
        return raw_url;
    }

    const size_t authority_begin = scheme_separator + 3;
    const size_t suffix_begin = raw_url.find_first_of("/?#", authority_begin);
    const size_t authority_end = suffix_begin == std::string::npos ? raw_url.size() : suffix_begin;
    const std::string authority = raw_url.substr(authority_begin, authority_end - authority_begin);
    const size_t at_sign = authority.rfind('@');
    if (at_sign == std::string::npos) {
        return raw_url;
    }

    const std::string userinfo = authority.substr(0, at_sign);
    const std::string host_part = authority.substr(at_sign + 1);
    const size_t colon = userinfo.find(':');
    const std::string sanitized_userinfo =
        colon == std::string::npos ? userinfo : userinfo.substr(0, colon) + ":******";

    std::string sanitized = raw_url.substr(0, authority_begin);
    sanitized += sanitized_userinfo;
    sanitized.push_back('@');
    sanitized += host_part;
    if (suffix_begin != std::string::npos) {
        sanitized += raw_url.substr(suffix_begin);
    }
    return sanitized;
}

bool url_contains_userinfo(const std::string& raw_url) {
    const size_t scheme_separator = raw_url.find("://");
    if (scheme_separator == std::string::npos) {
        return false;
    }

    const size_t authority_begin = scheme_separator + 3;
    const size_t authority_end = raw_url.find_first_of("/?#", authority_begin);
    const size_t end = authority_end == std::string::npos ? raw_url.size() : authority_end;
    return raw_url.find('@', authority_begin) != std::string::npos &&
           raw_url.find('@', authority_begin) < end;
}

void append_webdav_debug_log_locked(WebDavDebugLogState& state, const std::string& message) {
    if (!kWebDavDebugLogEnabled) {
        return;
    }

    std::ofstream output(webdav_debug_log_path(), std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (state.session_start == std::chrono::steady_clock::time_point::min()) {
        state.session_start = now;
    }

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.session_start).count();
    output << "[t+" << elapsed_ms << "ms]"
           << "[#" << (++state.sequence) << "] "
           << sanitize_debug_log_message(message)
           << '\n';
}

void reset_webdav_debug_log_session_locked(WebDavDebugLogState& state) {
    if (!kWebDavDebugLogEnabled) {
        return;
    }

    std::error_code error;
    const auto log_path = webdav_debug_log_path();
    std::filesystem::create_directories(log_path.parent_path(), error);

    std::ofstream output(log_path, std::ios::binary | std::ios::out | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    state.initialized = true;
    state.session_start = std::chrono::steady_clock::now();
    state.session_token = make_webdav_debug_session_token();

    output << "\n========== WEBDAV LOG SESSION BEGIN ==========\n";
    output << "[t+0ms][#" << (++state.sequence) << "] [webdav] log_format=2026-04-25g\n";
    output << "[t+0ms][#" << (++state.sequence) << "] [webdav] session_token=" << state.session_token << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [webdav] base_directory="
           << switchbox::core::AppConfigStore::paths().base_directory.string()
           << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [webdav] config_file="
           << switchbox::core::AppConfigStore::paths().config_file.string()
           << '\n';
    output << "[t+0ms][#" << (++state.sequence) << "] [webdav] transport_backend=libnx_ssl\n";
    output.flush();
}

void append_webdav_debug_log(const std::string& message) {
    if (!kWebDavDebugLogEnabled) {
        return;
    }

    auto& state = webdav_debug_log_state();
    std::scoped_lock lock(state.mutex);
    if (!state.initialized) {
        reset_webdav_debug_log_session_locked(state);
    }
    append_webdav_debug_log_locked(state, message);
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool contains_text(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string trim_slashes(std::string value) {
    value = trim(std::move(value));
    while (!value.empty() && (value.front() == '/' || value.front() == '\\')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }
    return value;
}

std::vector<std::string> split_relative_segments(std::string_view raw_path) {
    std::vector<std::string> segments;
    std::string current;

    const auto flush = [&segments, &current]() {
        std::string segment = trim_slashes(current);
        current.clear();

        if (segment.empty() || segment == ".") {
            return;
        }

        if (segment == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
            return;
        }

        segments.push_back(std::move(segment));
    };

    for (const char character : raw_path) {
        if (character == '/' || character == '\\') {
            flush();
            continue;
        }

        current.push_back(character);
    }

    flush();
    return segments;
}

std::string join_segments(const std::vector<std::string>& segments) {
    std::string joined;
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            joined.push_back('/');
        }
        joined += segments[index];
    }
    return joined;
}

int decode_hex_nibble(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return 10 + (character - 'a');
    }
    if (character >= 'A' && character <= 'F') {
        return 10 + (character - 'A');
    }
    return -1;
}

std::string percent_decode_component(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '%' && index + 2 < value.size()) {
            const int high = decode_hex_nibble(value[index + 1]);
            const int low = decode_hex_nibble(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }

        decoded.push_back(character == '+' ? ' ' : character);
    }

    return decoded;
}

bool is_unreserved(unsigned char character) {
    return std::isalnum(character) != 0 ||
           character == '-' ||
           character == '.' ||
           character == '_' ||
           character == '~';
}

std::string percent_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";

    std::string encoded;
    for (const unsigned char character : value) {
        if (is_unreserved(character)) {
            encoded.push_back(static_cast<char>(character));
            continue;
        }

        encoded.push_back('%');
        encoded.push_back(hex[(character >> 4) & 0x0F]);
        encoded.push_back(hex[character & 0x0F]);
    }

    return encoded;
}

std::string build_default_http_user_agent() {
    return "Mozilla/5.0 (X11; Linux x86_64; rv:49.0) Gecko/20100101 Firefox/49.0";
}

std::string encode_path_segments(const std::vector<std::string>& segments) {
    std::string encoded_path;

    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            encoded_path.push_back('/');
        }

        encoded_path += percent_encode(segments[index]);
    }

    return encoded_path;
}

std::string base64_encode(std::string_view value) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((value.size() + 2) / 3) * 4);

    size_t index = 0;
    while (index < value.size()) {
        const unsigned int octet_a = static_cast<unsigned char>(value[index++]);
        const unsigned int octet_b =
            index < value.size() ? static_cast<unsigned char>(value[index++]) : 0;
        const unsigned int octet_c =
            index < value.size() ? static_cast<unsigned char>(value[index++]) : 0;

        const unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        encoded.push_back(table[(triple >> 18) & 0x3F]);
        encoded.push_back(table[(triple >> 12) & 0x3F]);
        encoded.push_back(index - 1 < value.size() + 1 ? table[(triple >> 6) & 0x3F] : '=');
        encoded.push_back(index <= value.size() ? table[triple & 0x3F] : '=');
    }

    const size_t remainder = value.size() % 3;
    if (remainder > 0) {
        encoded[encoded.size() - 1] = '=';
        if (remainder == 1) {
            encoded[encoded.size() - 2] = '=';
        }
    }

    return encoded;
}

std::string normalize_header_name(std::string value) {
    value = to_lower(trim(std::move(value)));
    std::replace(value.begin(), value.end(), '_', '-');
    return value;
}

struct ParsedHttpUrl {
    std::string scheme;
    std::string authority;
    std::string host;
    std::string port;
    std::vector<std::string> decoded_path_segments;
    bool trailing_slash = false;
    bool valid = false;
};

bool split_http_authority(
    const std::string& scheme,
    const std::string& authority,
    std::string& host,
    std::string& port) {
    host.clear();
    port.clear();

    if (authority.empty()) {
        return false;
    }

    if (authority.front() == '[') {
        const size_t closing_bracket = authority.find(']');
        if (closing_bracket == std::string::npos) {
            return false;
        }

        host = authority.substr(1, closing_bracket - 1);
        if (host.empty()) {
            return false;
        }

        if (closing_bracket + 1 < authority.size()) {
            if (authority[closing_bracket + 1] != ':') {
                return false;
            }
            port = authority.substr(closing_bracket + 2);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }

    host = trim(host);
    port = trim(port);
    if (host.empty()) {
        return false;
    }

    if (port.empty()) {
        port = scheme == "https" ? "443" : "80";
    }

    if (!std::all_of(port.begin(), port.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return false;
    }

    return true;
}

std::optional<ParsedHttpUrl> parse_http_url(const std::string& raw_url) {
    const std::string url = trim(raw_url);
    const size_t scheme_separator = url.find("://");
    if (scheme_separator == std::string::npos) {
        return std::nullopt;
    }

    ParsedHttpUrl parsed;
    parsed.scheme = to_lower(url.substr(0, scheme_separator));
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        return std::nullopt;
    }

    const size_t authority_begin = scheme_separator + 3;
    size_t path_begin = url.find('/', authority_begin);
    if (path_begin == std::string::npos) {
        parsed.authority = url.substr(authority_begin);
        path_begin = url.size();
        parsed.trailing_slash = true;
    } else {
        parsed.authority = url.substr(authority_begin, path_begin - authority_begin);
    }

    if (parsed.authority.empty()) {
        return std::nullopt;
    }

    if (!split_http_authority(parsed.scheme, parsed.authority, parsed.host, parsed.port)) {
        return std::nullopt;
    }

    const size_t query_begin = url.find_first_of("?#", path_begin);
    const std::string path = query_begin == std::string::npos
        ? url.substr(path_begin)
        : url.substr(path_begin, query_begin - path_begin);

    parsed.trailing_slash = path.empty() || path.back() == '/';
    parsed.decoded_path_segments = split_relative_segments(percent_decode_component(path));
    parsed.valid = true;
    return parsed;
}

std::string build_url_from_segments(
    const ParsedHttpUrl& parsed_root_url,
    const std::vector<std::string>& decoded_segments,
    bool directory) {
    std::string url = parsed_root_url.scheme + "://" + parsed_root_url.authority + "/";
    url += encode_path_segments(decoded_segments);
    if (directory && !url.empty() && url.back() != '/') {
        url.push_back('/');
    }
    return url;
}

std::string build_request_target_from_segments(
    const std::vector<std::string>& decoded_segments,
    bool directory) {
    std::string target = "/";
    target += encode_path_segments(decoded_segments);
    if (directory && !target.empty() && target.back() != '/') {
        target.push_back('/');
    }
    return target.empty() ? "/" : target;
}

std::string build_authorized_url(
    const ParsedHttpUrl& parsed_root_url,
    std::string_view username,
    std::string_view password,
    const std::vector<std::string>& decoded_segments,
    bool directory) {
    std::string url = parsed_root_url.scheme + "://";

    const std::string trimmed_username = trim(std::string(username));
    if (!trimmed_username.empty()) {
        url += percent_encode(trimmed_username);

        const std::string trimmed_password = trim(std::string(password));
        if (!trimmed_password.empty()) {
            url.push_back(':');
            url += percent_encode(trimmed_password);
        }

        url.push_back('@');
    }

    url += parsed_root_url.authority;
    url.push_back('/');
    url += encode_path_segments(decoded_segments);
    if (directory && !url.empty() && url.back() != '/') {
        url.push_back('/');
    }
    return url;
}

std::vector<std::string> root_and_relative_segments(
    const WebDavSourceSettings& source,
    const std::string& relative_path) {
    const auto parsed = parse_http_url(source.url);
    if (!parsed.has_value()) {
        return {};
    }

    std::vector<std::string> segments = parsed->decoded_path_segments;
    const auto relative_segments = split_relative_segments(relative_path);
    segments.insert(segments.end(), relative_segments.begin(), relative_segments.end());
    return segments;
}

std::string local_name(const char* qualified_name) {
    if (qualified_name == nullptr) {
        return {};
    }

    std::string_view view(qualified_name);
    const size_t separator = view.find(':');
    return std::string(separator == std::string_view::npos ? view : view.substr(separator + 1));
}

const tinyxml2::XMLElement* first_child_local(
    const tinyxml2::XMLElement* parent,
    std::string_view expected_local_name) {
    if (parent == nullptr) {
        return nullptr;
    }

    for (auto* child = parent->FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
        if (local_name(child->Name()) == expected_local_name) {
            return child;
        }
    }

    return nullptr;
}

const tinyxml2::XMLElement* next_sibling_local(
    const tinyxml2::XMLElement* element,
    std::string_view expected_local_name) {
    if (element == nullptr) {
        return nullptr;
    }

    for (auto* sibling = element->NextSiblingElement(); sibling != nullptr; sibling = sibling->NextSiblingElement()) {
        if (local_name(sibling->Name()) == expected_local_name) {
            return sibling;
        }
    }

    return nullptr;
}

std::string child_text_local(const tinyxml2::XMLElement* parent, std::string_view expected_local_name) {
    const auto* child = first_child_local(parent, expected_local_name);
    if (child == nullptr || child->GetText() == nullptr) {
        return {};
    }
    return trim(child->GetText());
}

bool descendant_collection_flag(const tinyxml2::XMLElement* parent) {
    const auto* resource_type = first_child_local(parent, "resourcetype");
    if (resource_type == nullptr) {
        return false;
    }

    return first_child_local(resource_type, "collection") != nullptr;
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153 * (month + (month > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 +
        day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(day_of_era) - 719468;
}

std::int64_t parse_http_datetime(std::string value) {
    value = trim(std::move(value));
    if (value.empty()) {
        return 0;
    }

    std::tm parsed_time {};
    std::istringstream input(value);
    input >> std::get_time(&parsed_time, "%a, %d %b %Y %H:%M:%S");
    if (input.fail()) {
        return 0;
    }

    const std::int64_t days = days_from_civil(
        parsed_time.tm_year + 1900,
        static_cast<unsigned>(parsed_time.tm_mon + 1),
        static_cast<unsigned>(parsed_time.tm_mday));
    return days * 86400 +
           static_cast<std::int64_t>(parsed_time.tm_hour) * 3600 +
           static_cast<std::int64_t>(parsed_time.tm_min) * 60 +
           static_cast<std::int64_t>(parsed_time.tm_sec);
}

bool entry_name_less(const WebDavBrowserEntry& lhs, const WebDavBrowserEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory && !rhs.is_directory;
    }
    return to_lower(lhs.name) < to_lower(rhs.name);
}

bool entry_name_greater(const WebDavBrowserEntry& lhs, const WebDavBrowserEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory && !rhs.is_directory;
    }
    return to_lower(lhs.name) > to_lower(rhs.name);
}

template <typename ValueGetter>
bool entry_value_less(
    const WebDavBrowserEntry& lhs,
    const WebDavBrowserEntry& rhs,
    const ValueGetter& value_getter,
    bool ascending) {
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory && !rhs.is_directory;
    }

    const auto lhs_value = value_getter(lhs);
    const auto rhs_value = value_getter(rhs);
    if (lhs_value != rhs_value) {
        return ascending ? (lhs_value < rhs_value) : (lhs_value > rhs_value);
    }

    const std::string lhs_name = to_lower(lhs.name);
    const std::string rhs_name = to_lower(rhs.name);
    if (lhs_name != rhs_name) {
        return ascending ? (lhs_name < rhs_name) : (lhs_name > rhs_name);
    }

    return lhs.relative_path < rhs.relative_path;
}

void sort_entries(std::vector<WebDavBrowserEntry>& entries, const std::string& sort_order) {
    if (sort_order == "name_desc") {
        std::sort(entries.begin(), entries.end(), entry_name_greater);
        return;
    }

    if (sort_order == "date_asc") {
        std::sort(entries.begin(), entries.end(), [](const WebDavBrowserEntry& lhs, const WebDavBrowserEntry& rhs) {
            return entry_value_less(lhs, rhs, [](const WebDavBrowserEntry& entry) {
                return entry.modified_timestamp;
            }, true);
        });
        return;
    }

    if (sort_order == "date_desc") {
        std::sort(entries.begin(), entries.end(), [](const WebDavBrowserEntry& lhs, const WebDavBrowserEntry& rhs) {
            return entry_value_less(lhs, rhs, [](const WebDavBrowserEntry& entry) {
                return entry.modified_timestamp;
            }, false);
        });
        return;
    }

    if (sort_order == "size_asc") {
        std::sort(entries.begin(), entries.end(), [](const WebDavBrowserEntry& lhs, const WebDavBrowserEntry& rhs) {
            return entry_value_less(lhs, rhs, [](const WebDavBrowserEntry& entry) {
                return entry.size;
            }, true);
        });
        return;
    }

    if (sort_order == "size_desc") {
        std::sort(entries.begin(), entries.end(), [](const WebDavBrowserEntry& lhs, const WebDavBrowserEntry& rhs) {
            return entry_value_less(lhs, rhs, [](const WebDavBrowserEntry& entry) {
                return entry.size;
            }, false);
        });
        return;
    }

    std::sort(entries.begin(), entries.end(), entry_name_less);
}

std::optional<std::vector<std::string>> href_to_relative_segments(
    const std::string& href,
    const ParsedHttpUrl& parsed_root_url) {
    std::vector<std::string> href_segments;

    const std::string trimmed_href = trim(href);
    if (trimmed_href.empty()) {
        return std::nullopt;
    }

    if (starts_with(to_lower(trimmed_href), "http://") || starts_with(to_lower(trimmed_href), "https://")) {
        const auto parsed_href = parse_http_url(trimmed_href);
        if (!parsed_href.has_value()) {
            return std::nullopt;
        }

        if (to_lower(parsed_href->authority) != to_lower(parsed_root_url.authority)) {
            return std::nullopt;
        }

        href_segments = parsed_href->decoded_path_segments;
    } else if (trimmed_href.front() == '/') {
        href_segments = split_relative_segments(percent_decode_component(trimmed_href));
    } else {
        href_segments = split_relative_segments(percent_decode_component(trimmed_href));
        std::vector<std::string> absolute_segments = parsed_root_url.decoded_path_segments;
        absolute_segments.insert(absolute_segments.end(), href_segments.begin(), href_segments.end());
        href_segments = std::move(absolute_segments);
    }

    if (href_segments.size() < parsed_root_url.decoded_path_segments.size()) {
        return std::nullopt;
    }

    if (!std::equal(
            parsed_root_url.decoded_path_segments.begin(),
            parsed_root_url.decoded_path_segments.end(),
            href_segments.begin())) {
        return std::nullopt;
    }

    return std::vector<std::string>(
        href_segments.begin() + static_cast<std::ptrdiff_t>(parsed_root_url.decoded_path_segments.size()),
        href_segments.end());
}

#if SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT

constexpr int kWebDavNetworkTimeoutMs = 15000;
constexpr int kWebDavTransientRetryCount = 2;
constexpr int64_t kWebDavTransientRetryDelayNs = 250'000'000LL;
constexpr size_t kWebDavReadBufferSize = 16 * 1024;
constexpr size_t kWebDavMaxResponseBytes = 4 * 1024 * 1024;
constexpr size_t kWebDavMaxFileRangeBytes = 8 * 1024 * 1024;
constexpr size_t kWebDavMaxFileRangeResponseBytes = kWebDavMaxFileRangeBytes + 128 * 1024;

struct HttpResponseHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    int status_code = 0;
    std::string status_line;
    std::vector<HttpResponseHeader> headers;
    std::string body;
};

struct SwitchSslGlobalState {
    std::mutex mutex;
    bool initialized = false;
    Result init_result = 0;
};

SwitchSslGlobalState& switch_ssl_global_state() {
    static SwitchSslGlobalState state;
    return state;
}

void report_webdav_browse_progress(
    const std::function<void(const WebDavBrowseLoadProgress&)>& progress_callback,
    WebDavBrowseLoadStage stage,
    float progress) {
    if (!progress_callback) {
        return;
    }

    progress_callback(WebDavBrowseLoadProgress {
        .stage = stage,
        .progress = std::clamp(progress, 0.0f, 1.0f),
    });
}

bool is_transient_webdav_transport_error(const std::string& error_message) {
    const std::string lowered = to_lower(error_message);
    return contains_text(lowered, "resolve webdav host failed") ||
           contains_text(lowered, "connect webdav server failed") ||
           contains_text(lowered, "connect socket failed") ||
           contains_text(lowered, "timed out") ||
           contains_text(lowered, "temporarily unavailable") ||
           contains_text(lowered, "network is unreachable") ||
           contains_text(lowered, "resource temporarily unavailable") ||
           contains_text(lowered, "try again");
}

std::string switch_result_to_string(Result result) {
    std::ostringstream stream;
    stream << "0x"
           << std::uppercase
           << std::hex
           << std::setw(8)
           << std::setfill('0')
           << static_cast<uint32_t>(result)
           << std::dec
           << " (module=" << R_MODULE(result)
           << ", description=" << R_DESCRIPTION(result)
           << ")";
    return stream.str();
}

bool ensure_switch_ssl_initialized(std::string& error_message) {
    auto& state = switch_ssl_global_state();
    std::scoped_lock lock(state.mutex);
    if (state.initialized) {
        error_message.clear();
        return true;
    }

    state.init_result = sslInitialize(3);
    if (R_FAILED(state.init_result) &&
        state.init_result != MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)) {
        error_message = "Initialize Switch SSL service failed: " + switch_result_to_string(state.init_result);
        return false;
    }

    state.initialized = true;
    error_message.clear();
    return true;
}

struct WebDavConnectionContext {
    int socket_fd = -1;
    SslContext ssl_context {};
    SslConnection ssl_connection {};
    bool use_tls = false;
    bool ssl_context_open = false;
    bool ssl_connection_open = false;

    ~WebDavConnectionContext() {
        if (ssl_connection_open) {
            sslConnectionClose(&ssl_connection);
        }
        if (ssl_context_open) {
            sslContextClose(&ssl_context);
        }
        if (socket_fd >= 0) {
            close(socket_fd);
        }
    }
};

void configure_socket_timeouts(int socket_fd) {
    if (socket_fd < 0) {
        return;
    }

    timeval timeout {};
    timeout.tv_sec = kWebDavNetworkTimeoutMs / 1000;
    timeout.tv_usec = (kWebDavNetworkTimeoutMs % 1000) * 1000;

    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

std::optional<std::string> response_header_value(
    const std::vector<HttpResponseHeader>& headers,
    std::string_view name) {
    const std::string normalized = normalize_header_name(std::string(name));
    for (const auto& header : headers) {
        if (normalize_header_name(header.name) == normalized) {
            return header.value;
        }
    }
    return std::nullopt;
}

std::string build_propfind_request_text(
    const ParsedHttpUrl& parsed_url,
    const std::string& request_target,
    const std::string& authorization_header) {
    std::vector<std::string> header_fields = {
        "Host: " + parsed_url.authority,
        "User-Agent: " + build_default_http_user_agent(),
        "Accept: */*",
        "Accept-Encoding: identity",
        "Depth: 1",
        "Content-Type: text/xml; charset=utf-8",
        "Content-Length: 0",
        "Connection: close",
    };

    if (!authorization_header.empty()) {
        header_fields.push_back(authorization_header);
    }

    std::string request = "PROPFIND " + request_target + " HTTP/1.1\r\n";
    for (const std::string& header : header_fields) {
        request += trim(header);
        request += "\r\n";
    }
    request += "\r\n";
    return request;
}

std::string build_get_request_text(
    const ParsedHttpUrl& parsed_url,
    const std::string& request_target,
    const std::string& authorization_header,
    const std::string& range_header) {
    std::vector<std::string> header_fields = {
        "Host: " + parsed_url.authority,
        "User-Agent: " + build_default_http_user_agent(),
        "Accept: */*",
        "Accept-Encoding: identity",
        "Connection: close",
    };

    if (!authorization_header.empty()) {
        header_fields.push_back(authorization_header);
    }
    if (!range_header.empty()) {
        header_fields.push_back(range_header);
    }

    std::string request = "GET " + request_target + " HTTP/1.1\r\n";
    for (const std::string& header : header_fields) {
        request += trim(header);
        request += "\r\n";
    }
    request += "\r\n";
    return request;
}

std::string build_delete_request_text(
    const ParsedHttpUrl& parsed_url,
    const std::string& request_target,
    const std::string& authorization_header) {
    std::vector<std::string> header_fields = {
        "Host: " + parsed_url.authority,
        "User-Agent: " + build_default_http_user_agent(),
        "Accept: */*",
        "Accept-Encoding: identity",
        "Content-Length: 0",
        "Connection: close",
    };

    if (!authorization_header.empty()) {
        header_fields.push_back(authorization_header);
    }

    std::string request = "DELETE " + request_target + " HTTP/1.1\r\n";
    for (const std::string& header : header_fields) {
        request += trim(header);
        request += "\r\n";
    }
    request += "\r\n";
    return request;
}

bool parse_content_range_header(
    std::string_view raw_value,
    std::uintmax_t& out_start,
    std::uintmax_t& out_end,
    std::uintmax_t& out_total) {
    out_start = 0;
    out_end = 0;
    out_total = 0;

    const std::string value = trim(std::string(raw_value));
    if (!starts_with(value, "bytes ")) {
        return false;
    }

    const size_t slash = value.find('/');
    const size_t dash = value.find('-');
    if (dash == std::string::npos || slash == std::string::npos || dash <= 6 || slash <= dash + 1) {
        return false;
    }

    try {
        out_start = static_cast<std::uintmax_t>(std::stoull(value.substr(6, dash - 6)));
        out_end = static_cast<std::uintmax_t>(std::stoull(value.substr(dash + 1, slash - dash - 1)));
        out_total = static_cast<std::uintmax_t>(std::stoull(value.substr(slash + 1)));
    } catch (...) {
        return false;
    }

    return out_start <= out_end && out_end < out_total;
}

bool connect_tcp_socket(
    const ParsedHttpUrl& parsed_url,
    WebDavConnectionContext& connection,
    std::string& error_message) {
    std::string last_error = "Connect socket failed.";
    const auto try_connect = [&](const sockaddr* address, socklen_t address_length, int family) -> bool {
        const int socket_fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (socket_fd < 0) {
            last_error = "Create socket failed: " + std::string(std::strerror(errno));
            return false;
        }

        configure_socket_timeouts(socket_fd);
        if (connect(socket_fd, address, address_length) == 0) {
            connection.socket_fd = socket_fd;
            error_message.clear();
            return true;
        }

        last_error = "Connect socket failed: " + std::string(std::strerror(errno));
        close(socket_fd);
        return false;
    };

    in_addr ipv4_address {};
    if (inet_pton(AF_INET, parsed_url.host.c_str(), &ipv4_address) == 1) {
        append_webdav_debug_log("[webdav] dns literal_ipv4 host=" + parsed_url.host);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(std::stoi(parsed_url.port)));
        address.sin_addr = ipv4_address;
        if (try_connect(reinterpret_cast<const sockaddr*>(&address), sizeof(address), AF_INET)) {
            return true;
        }
    }

    const auto try_getaddrinfo = [&](int family, std::string_view label) -> bool {
        addrinfo hints {};
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* addresses = nullptr;
        const int lookup_result =
            getaddrinfo(parsed_url.host.c_str(), parsed_url.port.c_str(), &hints, &addresses);
        if (lookup_result != 0) {
            append_webdav_debug_log(
                "[webdav] dns getaddrinfo_failed mode=" + std::string(label) +
                " error=" + std::string(gai_strerror(lookup_result)));
            last_error = "Resolve WebDAV host failed: " + std::string(gai_strerror(lookup_result));
            return false;
        }

        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address_guard(addresses, freeaddrinfo);
        append_webdav_debug_log(
            "[webdav] dns getaddrinfo_ok mode=" + std::string(label) +
            " host=" + parsed_url.host);

        for (addrinfo* current = addresses; current != nullptr; current = current->ai_next) {
            if (try_connect(current->ai_addr, static_cast<socklen_t>(current->ai_addrlen), current->ai_family)) {
                return true;
            }
        }

        return false;
    };

    if (try_getaddrinfo(AF_UNSPEC, "af_unspec")) {
        return true;
    }
    if (try_getaddrinfo(AF_INET, "af_inet")) {
        return true;
    }

    append_webdav_debug_log("[webdav] dns gethostbyname_begin host=" + parsed_url.host);
    hostent* host_entry = gethostbyname(parsed_url.host.c_str());
    if (host_entry == nullptr || host_entry->h_addr_list == nullptr || host_entry->h_addr_list[0] == nullptr) {
        last_error = "Resolve WebDAV host failed: gethostbyname returned no IPv4 address.";
        append_webdav_debug_log("[webdav] dns gethostbyname_failed");
        error_message = last_error;
        return false;
    }

    const uint16_t port = static_cast<uint16_t>(std::stoi(parsed_url.port));
    for (char** address_ptr = host_entry->h_addr_list; *address_ptr != nullptr; ++address_ptr) {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        std::memcpy(&address.sin_addr, *address_ptr, sizeof(in_addr));

        char ip_buffer[16] {};
        if (inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer)) != nullptr) {
            append_webdav_debug_log("[webdav] dns gethostbyname_ip=" + std::string(ip_buffer));
        }

        if (try_connect(reinterpret_cast<const sockaddr*>(&address), sizeof(address), AF_INET)) {
            return true;
        }
    }

    error_message = last_error;
    return false;
}

u32 webdav_ssl_api_version_for_hos() {
    if (hosversionAtLeast(19, 0, 0)) {
        return 5;
    }
    if (hosversionAtLeast(17, 0, 0)) {
        return 4;
    }
    if (hosversionAtLeast(14, 0, 0)) {
        return 3;
    }
    if (hosversionAtLeast(12, 0, 0)) {
        return 2;
    }
    if (hosversionAtLeast(11, 0, 0)) {
        return 1;
    }
    return 0;
}

u32 build_webdav_ssl_version_mask() {
    const u32 api_version = webdav_ssl_api_version_for_hos();
    u32 ssl_version = SslVersion_Auto;
    if (api_version > 0) {
        ssl_version |= (api_version << 24);
    }
    return ssl_version;
}

bool connect_webdav_transport(
    const ParsedHttpUrl& parsed_url,
    WebDavConnectionContext& connection,
    std::string& error_message) {
    error_message.clear();
    if (!connect_tcp_socket(parsed_url, connection, error_message)) {
        error_message = "Connect WebDAV server failed: " + error_message;
        return false;
    }

    connection.use_tls = parsed_url.scheme == "https";
    if (!connection.use_tls) {
        return true;
    }

    if (!ensure_switch_ssl_initialized(error_message)) {
        return false;
    }

    if (hosversionAtLeast(14, 0, 0)) {
        const Result clear_tls12_fallback_result = sslClearTls12FallbackFlag();
        append_webdav_debug_log(
            "[webdav] tls clear_tls12_fallback result=" +
            switch_result_to_string(clear_tls12_fallback_result));
    }

    const u32 hos_version = hosversionGet();
    const u32 ssl_api_version = webdav_ssl_api_version_for_hos();
    const u32 ssl_version = build_webdav_ssl_version_mask();
    append_webdav_debug_log(
        "[webdav] tls context hos=" +
        std::to_string(HOSVER_MAJOR(hos_version)) + "." +
        std::to_string(HOSVER_MINOR(hos_version)) + "." +
        std::to_string(HOSVER_MICRO(hos_version)) +
        " api_version=" + std::to_string(ssl_api_version) +
        " ssl_version_mask=0x" +
        [&]() {
            std::ostringstream stream;
            stream << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << ssl_version;
            return stream.str();
        }());
    Result rc = sslCreateContext(&connection.ssl_context, ssl_version);
    if (R_FAILED(rc)) {
        error_message = "Create Switch SSL context failed: " + switch_result_to_string(rc);
        return false;
    }
    connection.ssl_context_open = true;

    if (hosversionAtLeast(15, 0, 0)) {
        rc = sslContextCreateConnectionForSystem(&connection.ssl_context, &connection.ssl_connection);
        append_webdav_debug_log(
            "[webdav] tls create_connection mode=system result=" + switch_result_to_string(rc));
    } else {
        rc = sslContextCreateConnection(&connection.ssl_context, &connection.ssl_connection);
        append_webdav_debug_log(
            "[webdav] tls create_connection mode=default result=" + switch_result_to_string(rc));
    }
    if (R_FAILED(rc)) {
        error_message = "Create Switch SSL connection failed: " + switch_result_to_string(rc);
        return false;
    }
    connection.ssl_connection_open = true;

    rc = sslConnectionSetOption(&connection.ssl_connection, SslOptionType_DoNotCloseSocket, true);
    if (R_FAILED(rc)) {
        error_message = "Configure Switch SSL socket ownership failed: " + switch_result_to_string(rc);
        return false;
    }

    errno = 0;
    const int detached_socket =
        socketSslConnectionSetSocketDescriptor(&connection.ssl_connection, connection.socket_fd);
    if (detached_socket < 0 && errno != ENOENT) {
        const Result socket_result = socketGetLastResult();
        if (R_FAILED(socket_result)) {
            error_message = "Attach Switch SSL socket failed: " + switch_result_to_string(socket_result);
        } else {
            error_message = "Attach Switch SSL socket failed: " + std::string(std::strerror(errno));
        }
        return false;
    }
    if (detached_socket >= 0 && detached_socket != connection.socket_fd) {
        close(detached_socket);
    }

    rc = sslConnectionSetHostName(
        &connection.ssl_connection,
        parsed_url.host.c_str(),
        static_cast<u32>(parsed_url.host.size() + 1));
    if (R_FAILED(rc)) {
        error_message = "Set Switch SSL hostname failed: " + switch_result_to_string(rc);
        return false;
    }

    rc = sslConnectionSetIoMode(&connection.ssl_connection, SslIoMode_Blocking);
    if (R_FAILED(rc)) {
        error_message = "Set Switch SSL I/O mode failed: " + switch_result_to_string(rc);
        return false;
    }

    rc = sslConnectionSetIoTimeout(&connection.ssl_connection, kWebDavNetworkTimeoutMs);
    if (R_FAILED(rc)) {
        error_message = "Set Switch SSL timeout failed: " + switch_result_to_string(rc);
        return false;
    }

    u32 verify_option = 0;
    const Result verify_option_result =
        sslConnectionGetVerifyOption(&connection.ssl_connection, &verify_option);
    append_webdav_debug_log(
        "[webdav] tls verify_option result=" + switch_result_to_string(verify_option_result) +
        " value=" + std::to_string(verify_option));

    u32 cert_buffer_size = 0;
    u32 total_certs = 0;
    rc = sslConnectionDoHandshake(
        &connection.ssl_connection,
        &cert_buffer_size,
        &total_certs,
        nullptr,
        0);
    if (R_FAILED(rc)) {
        const Result verify_cert_error = sslConnectionGetVerifyCertError(&connection.ssl_connection);
        append_webdav_debug_log(
            "[webdav] tls verify_cert_error result=" + switch_result_to_string(verify_cert_error));
        error_message = "WebDAV TLS handshake failed: " + switch_result_to_string(rc);
        return false;
    }

    return true;
}

bool parse_http_response_text(
    const std::string& response_text,
    HttpResponse& response,
    bool& complete_body,
    std::string& error_message);

bool write_all_to_transport(
    WebDavConnectionContext& connection,
    const std::string& request_text,
    std::string& error_message) {
    size_t offset = 0;
    while (offset < request_text.size()) {
        if (connection.use_tls) {
            u32 written_size = 0;
            const Result rc = sslConnectionWrite(
                &connection.ssl_connection,
                request_text.data() + offset,
                static_cast<u32>(request_text.size() - offset),
                &written_size);
            if (R_FAILED(rc)) {
                error_message = "Write WebDAV request failed: " + switch_result_to_string(rc);
                return false;
            }
            if (written_size == 0) {
                error_message = "Write WebDAV request failed: zero-byte TLS write.";
                return false;
            }
            offset += written_size;
        } else {
            const ssize_t write_result =
                send(connection.socket_fd, request_text.data() + offset, request_text.size() - offset, 0);
            if (write_result < 0) {
                error_message = "Write WebDAV request failed: " + std::string(std::strerror(errno));
                return false;
            }
            if (write_result == 0) {
                error_message = "Write WebDAV request failed: zero-byte socket write.";
                return false;
            }
            offset += static_cast<size_t>(write_result);
        }
    }

    return true;
}

bool read_http_response_from_transport(
    WebDavConnectionContext& connection,
    size_t max_response_bytes,
    HttpResponse& response,
    std::string& error_message) {
    std::string raw_response;
    std::array<unsigned char, kWebDavReadBufferSize> buffer {};
    while (raw_response.size() < max_response_bytes) {
        if (connection.use_tls) {
            u32 read_size = 0;
            const Result rc =
                sslConnectionRead(&connection.ssl_connection, buffer.data(), buffer.size(), &read_size);
            if (R_FAILED(rc)) {
                error_message = "Read WebDAV directory failed: " + switch_result_to_string(rc);
                return false;
            }
            if (read_size == 0) {
                break;
            }
            raw_response.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(read_size));
        } else {
            const ssize_t read_result = recv(connection.socket_fd, buffer.data(), buffer.size(), 0);
            if (read_result > 0) {
                raw_response.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(read_result));
                continue;
            }
            if (read_result == 0) {
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            error_message = "Read WebDAV directory failed: " + std::string(std::strerror(errno));
            return false;
        }
    }

    if (raw_response.size() >= max_response_bytes) {
        error_message = "Read WebDAV directory failed: response too large.";
        return false;
    }

    bool complete_body = false;
    if (!parse_http_response_text(raw_response, response, complete_body, error_message)) {
        return false;
    }

    if (!complete_body) {
        error_message = "Incomplete WebDAV HTTP response body.";
        return false;
    }

    return true;
}

size_t find_http_header_end(const std::string& response_text, size_t& separator_length) {
    size_t position = response_text.find("\r\n\r\n");
    if (position != std::string::npos) {
        separator_length = 4;
        return position;
    }

    position = response_text.find("\n\n");
    if (position != std::string::npos) {
        separator_length = 2;
        return position;
    }

    separator_length = 0;
    return std::string::npos;
}

bool decode_chunked_http_body(
    std::string_view encoded_body,
    std::string& decoded_body,
    bool& complete_body,
    std::string& error_message) {
    decoded_body.clear();
    complete_body = false;
    error_message.clear();

    size_t position = 0;
    while (position < encoded_body.size()) {
        size_t line_end = encoded_body.find("\r\n", position);
        size_t separator_length = 2;
        if (line_end == std::string::npos) {
            line_end = encoded_body.find('\n', position);
            separator_length = 1;
        }
        if (line_end == std::string::npos) {
            return true;
        }

        std::string chunk_length_text = trim(std::string(encoded_body.substr(position, line_end - position)));
        const size_t semicolon = chunk_length_text.find(';');
        if (semicolon != std::string::npos) {
            chunk_length_text.erase(semicolon);
            chunk_length_text = trim(std::move(chunk_length_text));
        }
        if (chunk_length_text.empty()) {
            error_message = "Invalid chunked WebDAV response: empty chunk length.";
            return false;
        }

        unsigned long chunk_length = 0;
        try {
            chunk_length = std::stoul(chunk_length_text, nullptr, 16);
        } catch (...) {
            error_message = "Invalid chunked WebDAV response: bad chunk length.";
            return false;
        }

        position = line_end + separator_length;
        if (encoded_body.size() < position + chunk_length) {
            return true;
        }

        decoded_body.append(encoded_body.substr(position, chunk_length));
        position += chunk_length;

        if (encoded_body.size() < position + 1) {
            return true;
        }

        if (encoded_body.compare(position, 2, "\r\n") == 0) {
            position += 2;
        } else if (encoded_body[position] == '\n') {
            position += 1;
        } else {
            error_message = "Invalid chunked WebDAV response: missing chunk terminator.";
            return false;
        }

        if (chunk_length == 0) {
            if (position >= encoded_body.size()) {
                complete_body = true;
                return true;
            }

            while (position < encoded_body.size()) {
                size_t trailer_end = encoded_body.find("\r\n", position);
                size_t trailer_separator_length = 2;
                if (trailer_end == std::string::npos) {
                    trailer_end = encoded_body.find('\n', position);
                    trailer_separator_length = 1;
                }
                if (trailer_end == std::string::npos) {
                    return true;
                }
                if (trailer_end == position) {
                    complete_body = true;
                    return true;
                }
                position = trailer_end + trailer_separator_length;
            }
            return true;
        }
    }

    return true;
}

bool parse_http_response_text(
    const std::string& response_text,
    HttpResponse& response,
    bool& complete_body,
    std::string& error_message) {
    response = HttpResponse {};
    complete_body = false;
    error_message.clear();

    size_t header_separator_length = 0;
    const size_t header_end = find_http_header_end(response_text, header_separator_length);
    if (header_end == std::string::npos) {
        error_message = "Incomplete WebDAV HTTP response headers.";
        return false;
    }

    std::string header_text = response_text.substr(0, header_end);
    const std::string raw_body = response_text.substr(header_end + header_separator_length);

    std::replace(header_text.begin(), header_text.end(), '\r', '\n');
    std::istringstream header_stream(header_text);

    if (!std::getline(header_stream, response.status_line)) {
        error_message = "Missing WebDAV HTTP status line.";
        return false;
    }
    response.status_line = trim(std::move(response.status_line));

    {
        std::istringstream status_line_stream(response.status_line);
        std::string http_version;
        status_line_stream >> http_version >> response.status_code;
        if (http_version.empty() || response.status_code <= 0) {
            error_message = "Invalid WebDAV HTTP status line.";
            return false;
        }
    }

    std::string line;
    while (std::getline(header_stream, line)) {
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        response.headers.push_back(HttpResponseHeader {
            .name = trim(line.substr(0, colon)),
            .value = trim(line.substr(colon + 1)),
        });
    }

    const std::string transfer_encoding =
        to_lower(response_header_value(response.headers, "Transfer-Encoding").value_or(std::string {}));
    const std::string content_length_text =
        trim(response_header_value(response.headers, "Content-Length").value_or(std::string {}));

    if (transfer_encoding.find("chunked") != std::string::npos) {
        std::string decoded_body;
        if (!decode_chunked_http_body(raw_body, decoded_body, complete_body, error_message)) {
            return false;
        }
        if (!complete_body) {
            error_message = "Incomplete chunked WebDAV HTTP response body.";
            return false;
        }
        response.body = std::move(decoded_body);
        return true;
    }

    if (!content_length_text.empty()) {
        size_t content_length = 0;
        try {
            content_length = static_cast<size_t>(std::stoull(content_length_text));
        } catch (...) {
            error_message = "Invalid WebDAV Content-Length response header.";
            return false;
        }

        if (raw_body.size() < content_length) {
            error_message = "Incomplete WebDAV HTTP response body.";
            return false;
        }

        response.body.assign(raw_body.data(), content_length);
        complete_body = true;
        return true;
    }

    response.body = raw_body;
    complete_body = true;
    return true;
}

std::string fetch_propfind_response_once(
    const std::string& url,
    const std::string& authorization_header,
    std::string& error_message) {
    error_message.clear();
    append_webdav_debug_log(
        "[webdav] request begin url=" + sanitize_url_for_debug_log(url) +
        " embedded_userinfo=" + std::string(url_contains_userinfo(url) ? "true" : "false") +
        " auth_header=" + std::string(authorization_header.empty() ? "false" : "true"));

    const auto parsed_url = parse_http_url(url);
    if (!parsed_url.has_value()) {
        error_message = "Open WebDAV directory failed: invalid URL.";
        append_webdav_debug_log("[webdav] request invalid_url");
        return {};
    }

    const std::string request_target =
        build_request_target_from_segments(parsed_url->decoded_path_segments, true);
    append_webdav_debug_log(
        "[webdav] transport prepare scheme=" + parsed_url->scheme +
        " host=" + parsed_url->host +
        " port=" + parsed_url->port +
        " target=" + request_target);

    WebDavConnectionContext connection;
    if (!connect_webdav_transport(parsed_url.value(), connection, error_message)) {
        error_message = "Open WebDAV directory failed: " + error_message;
        append_webdav_debug_log(
            "[webdav] request connect_failed url=" + sanitize_url_for_debug_log(url) +
            " error=" + error_message);
        return {};
    }

    append_webdav_debug_log(
        "[webdav] transport connected tls=" + std::string(connection.use_tls ? "true" : "false"));
    if (connection.use_tls) {
        SslCipherInfo cipher_info {};
        if (R_SUCCEEDED(sslConnectionGetCipherInfo(&connection.ssl_connection, &cipher_info))) {
            append_webdav_debug_log(
                "[webdav] tls cipher=" + trim(cipher_info.cipher) +
                " protocol=" + trim(cipher_info.protocol_version));
        }
    }

    const std::string request_text =
        build_propfind_request_text(parsed_url.value(), request_target, authorization_header);
    if (!write_all_to_transport(connection, request_text, error_message)) {
        error_message = "Open WebDAV directory failed: " + error_message;
        append_webdav_debug_log(
            "[webdav] request write_failed url=" + sanitize_url_for_debug_log(url) +
            " error=" + error_message);
        return {};
    }

    append_webdav_debug_log(
        "[webdav] request headers host=" + parsed_url->authority +
        " depth=1 auth_header=" + std::string(authorization_header.empty() ? "false" : "true"));

    HttpResponse response;
    if (!read_http_response_from_transport(connection, kWebDavMaxResponseBytes, response, error_message)) {
        error_message = "Open WebDAV directory failed: " + error_message;
        append_webdav_debug_log(
            "[webdav] request parse_failed url=" + sanitize_url_for_debug_log(url) +
            " error=" + error_message);
        return {};
    }

    append_webdav_debug_log(
        "[webdav] response status=" + std::to_string(response.status_code) +
        " line=" + response.status_line +
        " body_bytes=" + std::to_string(response.body.size()) +
        " complete=true");

    if (const auto content_type = response_header_value(response.headers, "Content-Type"); content_type.has_value()) {
        append_webdav_debug_log("[webdav] response content_type=" + *content_type);
    }

    if (response.status_code < 200 || response.status_code >= 300) {
        error_message =
            "Open WebDAV directory failed: HTTP " + std::to_string(response.status_code);
        append_webdav_debug_log(
            "[webdav] request http_error status=" + std::to_string(response.status_code) +
            " body_preview=" + summarize_body_for_debug_log(response.body));
        return {};
    }

    append_webdav_debug_log(
        "[webdav] request success url=" + sanitize_url_for_debug_log(url) +
        " bytes=" + std::to_string(response.body.size()));
    return response.body;
}

std::string fetch_propfind_response(
    const std::string& request_url,
    const std::string& authorized_request_url,
    const std::string& authorization_header,
    std::string& error_message) {
    (void)authorized_request_url;

    std::vector<std::string> attempts;
    if (!authorization_header.empty()) {
        attempts.push_back(authorization_header);
    }
    attempts.emplace_back();

    std::string collected_errors;
    size_t attempt_index = 0;
    for (const std::string& attempt_authorization_header : attempts) {
        if (attempt_index > 0 && attempt_authorization_header.empty() && authorization_header.empty()) {
            continue;
        }

        const bool auth_retry_allowed = !attempt_authorization_header.empty();
        for (int transport_attempt = 1; transport_attempt <= kWebDavTransientRetryCount; ++transport_attempt) {
            ++attempt_index;
            append_webdav_debug_log(
                "[webdav] attempt dispatch index=" + std::to_string(attempt_index) +
                " transport_attempt=" + std::to_string(transport_attempt) +
                "/" + std::to_string(kWebDavTransientRetryCount) +
                " url=" + sanitize_url_for_debug_log(request_url) +
                " embedded_userinfo=false auth_header=" +
                std::string(attempt_authorization_header.empty() ? "false" : "true"));

            std::string attempt_error;
            const std::string body =
                fetch_propfind_response_once(request_url, attempt_authorization_header, attempt_error);
            if (attempt_error.empty()) {
                error_message.clear();
                append_webdav_debug_log(
                    "[webdav] attempt success index=" + std::to_string(attempt_index) +
                    " body_preview=" + summarize_body_for_debug_log(body));
                return body;
            }

            if (!collected_errors.empty()) {
                collected_errors += " | ";
            }
            collected_errors += attempt_error;
            append_webdav_debug_log(
                "[webdav] attempt failed index=" + std::to_string(attempt_index) +
                " error=" + attempt_error);

            const bool should_retry_transport =
                transport_attempt < kWebDavTransientRetryCount &&
                is_transient_webdav_transport_error(attempt_error);
            if (should_retry_transport) {
                append_webdav_debug_log(
                    "[webdav] attempt retry_scheduled index=" + std::to_string(attempt_index) +
                    " delay_ms=250");
                svcSleepThread(kWebDavTransientRetryDelayNs);
                continue;
            }

            if (auth_retry_allowed) {
                const std::string lowered_error = to_lower(attempt_error);
                if (lowered_error.find("http 401") == std::string::npos &&
                    lowered_error.find("http 403") == std::string::npos) {
                    break;
                }
            }

            break;
        }
    }

    error_message = collected_errors.empty() ? "Open WebDAV directory failed: I/O error" : collected_errors;
    append_webdav_debug_log("[webdav] all_attempts_failed error=" + error_message);
    return {};
}

std::string build_basic_auth_header_for_source(const WebDavSourceSettings& source) {
    const std::string username = trim(source.username);
    if (username.empty()) {
        return {};
    }

    return "Authorization: Basic " + base64_encode(username + ":" + trim(source.password));
}

WebDavFileReadResult fetch_webdav_file_range_once(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::uintmax_t offset,
    std::size_t max_bytes,
    const std::string& authorization_header) {
    WebDavFileReadResult result;
    result.backend_available = true;

    const auto parsed_root_url = parse_http_url(source.url);
    if (!parsed_root_url.has_value()) {
        result.error_message = "Open WebDAV file failed: invalid URL.";
        return result;
    }

    const std::vector<std::string> segments = root_and_relative_segments(source, relative_path);
    const std::string request_url = build_url_from_segments(parsed_root_url.value(), segments, false);
    const std::string request_target = build_request_target_from_segments(segments, false);
    const std::uintmax_t clamped_max_bytes =
        std::min<std::uintmax_t>(std::max<std::size_t>(1, max_bytes), kWebDavMaxFileRangeBytes);
    const std::uintmax_t range_end = offset + clamped_max_bytes - 1;
    const std::string range_header =
        "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(range_end);

    append_webdav_debug_log(
        "[webdav][file] request begin path=" + relative_path +
        " offset=" + std::to_string(offset) +
        " max_bytes=" + std::to_string(clamped_max_bytes) +
        " url=" + sanitize_url_for_debug_log(request_url));

    WebDavConnectionContext connection;
    std::string transport_error;
    if (!connect_webdav_transport(parsed_root_url.value(), connection, transport_error)) {
        result.error_message = "Open WebDAV file failed: " + transport_error;
        append_webdav_debug_log("[webdav][file] connect_failed error=" + result.error_message);
        return result;
    }

    append_webdav_debug_log(
        "[webdav][file] transport connected tls=" + std::string(connection.use_tls ? "true" : "false"));
    if (connection.use_tls) {
        SslCipherInfo cipher_info {};
        if (R_SUCCEEDED(sslConnectionGetCipherInfo(&connection.ssl_connection, &cipher_info))) {
            append_webdav_debug_log(
                "[webdav][file] tls cipher=" + trim(cipher_info.cipher) +
                " protocol=" + trim(cipher_info.protocol_version));
        }
    }

    const std::string request_text =
        build_get_request_text(parsed_root_url.value(), request_target, authorization_header, range_header);
    if (!write_all_to_transport(connection, request_text, transport_error)) {
        result.error_message = "Open WebDAV file failed: " + transport_error;
        append_webdav_debug_log("[webdav][file] write_failed error=" + result.error_message);
        return result;
    }

    HttpResponse response;
    if (!read_http_response_from_transport(connection, kWebDavMaxFileRangeResponseBytes, response, transport_error)) {
        result.error_message = "Open WebDAV file failed: " + transport_error;
        append_webdav_debug_log("[webdav][file] response_failed error=" + result.error_message);
        return result;
    }

    append_webdav_debug_log(
        "[webdav][file] response status=" + std::to_string(response.status_code) +
        " body_bytes=" + std::to_string(response.body.size()));

    if (response.status_code != 200 && response.status_code != 206) {
        result.error_message = "Open WebDAV file failed: HTTP " + std::to_string(response.status_code);
        append_webdav_debug_log(
            "[webdav][file] http_error status=" + std::to_string(response.status_code) +
            " body_preview=" + summarize_body_for_debug_log(response.body));
        return result;
    }

    std::uintmax_t parsed_start = offset;
    std::uintmax_t parsed_end = offset;
    std::uintmax_t parsed_total = 0;
    bool has_content_range = false;
    if (const auto content_range = response_header_value(response.headers, "Content-Range"); content_range.has_value()) {
        has_content_range = parse_content_range_header(*content_range, parsed_start, parsed_end, parsed_total);
    }

    if (has_content_range) {
        result.file_size = parsed_total;
        result.response_offset = parsed_start;
    } else {
        result.response_offset = offset;
        if (const auto content_length = response_header_value(response.headers, "Content-Length"); content_length.has_value()) {
            try {
                result.file_size = offset + static_cast<std::uintmax_t>(std::stoull(*content_length));
            } catch (...) {
                result.file_size = offset + response.body.size();
            }
        } else {
            result.file_size = offset + response.body.size();
        }
    }

    result.bytes.assign(response.body.begin(), response.body.end());
    if (result.response_offset + result.bytes.size() >= result.file_size) {
        result.eof = true;
    }
    result.success = true;
    return result;
}

#endif

}  // namespace

std::string webdav_source_root_label(const WebDavSourceSettings& source) {
    const std::string title = trim(source.title);
    if (!title.empty()) {
        return title;
    }

    return trim(source.url);
}

std::string webdav_join_relative_path(const std::string& base_path, const std::string& child_name) {
    std::vector<std::string> segments = split_relative_segments(base_path);
    const auto child_segments = split_relative_segments(child_name);
    segments.insert(segments.end(), child_segments.begin(), child_segments.end());
    return join_segments(segments);
}

std::string webdav_parent_relative_path(const std::string& relative_path) {
    auto segments = split_relative_segments(relative_path);
    if (!segments.empty()) {
        segments.pop_back();
    }
    return join_segments(segments);
}

std::string webdav_display_path(const WebDavSourceSettings& source, const std::string& relative_path) {
    const std::string root_label = webdav_source_root_label(source);
    const std::string normalized_relative_path = webdav_join_relative_path({}, relative_path);
    if (normalized_relative_path.empty()) {
        return root_label;
    }

    if (root_label.empty()) {
        return normalized_relative_path;
    }

    return root_label + "/" + normalized_relative_path;
}

std::string webdav_file_url(const WebDavSourceSettings& source, const std::string& relative_path) {
    const auto parsed_root_url = parse_http_url(source.url);
    if (!parsed_root_url.has_value()) {
        return {};
    }

    return build_url_from_segments(parsed_root_url.value(), root_and_relative_segments(source, relative_path), false);
}

std::string webdav_authorized_file_url(const WebDavSourceSettings& source, const std::string& relative_path) {
    const auto parsed_root_url = parse_http_url(source.url);
    if (!parsed_root_url.has_value()) {
        return {};
    }

    return build_authorized_url(
        parsed_root_url.value(),
        source.username,
        source.password,
        root_and_relative_segments(source, relative_path),
        false);
}

std::string webdav_directory_url(const WebDavSourceSettings& source, const std::string& relative_path) {
    const auto parsed_root_url = parse_http_url(source.url);
    if (!parsed_root_url.has_value()) {
        return {};
    }

    return build_url_from_segments(parsed_root_url.value(), root_and_relative_segments(source, relative_path), true);
}

std::string webdav_build_basic_auth_header(const WebDavSourceSettings& source) {
    const std::string username = trim(source.username);
    if (username.empty()) {
        return {};
    }

    return "Authorization: Basic " + base64_encode(username + ":" + trim(source.password));
}

WebDavBrowserResult browse_webdav_directory(
    const WebDavSourceSettings& source,
    const GeneralSettings& general,
    const std::string& relative_path,
    bool include_all_entries,
    std::function<void(const WebDavBrowseLoadProgress&)> progress_callback) {
    WebDavBrowserResult result;
    result.root_label = webdav_source_root_label(source);
    result.requested_path = webdav_join_relative_path({}, relative_path);
    result.resolved_path = result.requested_path;
    result.normalized_extensions = normalize_playable_extensions(general.playable_extensions);

#if !SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT
    result.backend_available = false;
    result.error_message = "The WebDAV browser backend is not available in this build.";
    return result;
#else
    result.backend_available = true;
    report_webdav_browse_progress(progress_callback, WebDavBrowseLoadStage::Starting, 0.08f);
    append_webdav_debug_log(
        "[webdav] browse begin source_key=" + source.key +
        " title=" + webdav_source_root_label(source) +
        " relative_path=" + result.requested_path +
        " root_url=" + sanitize_url_for_debug_log(source.url));

    const auto parsed_root_url = parse_http_url(source.url);
    if (!parsed_root_url.has_value()) {
        result.error_message = "Invalid WebDAV root URL.";
        append_webdav_debug_log("[webdav] browse invalid_root_url");
        return result;
    }

    const std::string request_url = webdav_directory_url(source, result.requested_path);
    if (request_url.empty()) {
        result.error_message = "Unable to resolve the requested WebDAV directory URL.";
        append_webdav_debug_log("[webdav] browse request_url_empty");
        return result;
    }

    const std::string authorized_request_url = build_authorized_url(
        parsed_root_url.value(),
        source.username,
        source.password,
        root_and_relative_segments(source, result.requested_path),
        true);
    append_webdav_debug_log(
        "[webdav] browse prepared request_url=" + sanitize_url_for_debug_log(request_url) +
        " authorized_request_url=" + sanitize_url_for_debug_log(authorized_request_url) +
        " auth_header=" + std::string(webdav_build_basic_auth_header(source).empty() ? "false" : "true"));

    std::string error_message;
    report_webdav_browse_progress(progress_callback, WebDavBrowseLoadStage::OpeningConnection, 0.28f);
    const std::string response_body =
        fetch_propfind_response(
            request_url,
            authorized_request_url,
            webdav_build_basic_auth_header(source),
            error_message);
    if (!error_message.empty()) {
        result.error_message = std::move(error_message);
        append_webdav_debug_log("[webdav] browse failed error=" + result.error_message);
        return result;
    }

    report_webdav_browse_progress(progress_callback, WebDavBrowseLoadStage::ParsingResponse, 0.76f);
    tinyxml2::XMLDocument document;
    if (document.Parse(response_body.data(), response_body.size()) != tinyxml2::XML_SUCCESS) {
        result.error_message = "Failed to parse the WebDAV directory response.";
        append_webdav_debug_log(
            "[webdav] browse xml_parse_failed body_preview=" +
            summarize_body_for_debug_log(response_body));
        return result;
    }

    const auto* root = document.RootElement();
    if (root == nullptr || local_name(root->Name()) != "multistatus") {
        result.error_message = "The WebDAV server returned an unexpected directory response.";
        append_webdav_debug_log(
            "[webdav] browse unexpected_root root=" +
            std::string(root == nullptr || root->Name() == nullptr ? "<null>" : root->Name()) +
            " body_preview=" + summarize_body_for_debug_log(response_body));
        return result;
    }

    for (auto* response = first_child_local(root, "response");
         response != nullptr;
         response = next_sibling_local(response, "response")) {
        const std::string href = child_text_local(response, "href");
        const auto relative_segments = href_to_relative_segments(href, parsed_root_url.value());
        if (!relative_segments.has_value()) {
            continue;
        }

        const std::string resolved_relative_path = join_segments(relative_segments.value());
        if (resolved_relative_path == result.requested_path) {
            continue;
        }

        const tinyxml2::XMLElement* chosen_prop = nullptr;
        for (auto* propstat = first_child_local(response, "propstat");
             propstat != nullptr;
             propstat = next_sibling_local(propstat, "propstat")) {
            const std::string status = child_text_local(propstat, "status");
            if (!status.empty() && status.find(" 200 ") == std::string::npos) {
                continue;
            }

            chosen_prop = first_child_local(propstat, "prop");
            if (chosen_prop != nullptr) {
                break;
            }
        }

        if (chosen_prop == nullptr) {
            continue;
        }

        const bool is_directory = descendant_collection_flag(chosen_prop);
        const std::string display_name = child_text_local(chosen_prop, "displayname");
        const std::string size_text = child_text_local(chosen_prop, "getcontentlength");
        const std::string modified_text = child_text_local(chosen_prop, "getlastmodified");

        std::string entry_name = trim(display_name);
        if (entry_name.empty()) {
            const auto normalized_segments = split_relative_segments(resolved_relative_path);
            if (!normalized_segments.empty()) {
                entry_name = normalized_segments.back();
            }
        }
        if (entry_name.empty()) {
            continue;
        }

        WebDavBrowserEntry entry;
        entry.name = std::move(entry_name);
        entry.relative_path = resolved_relative_path;
        entry.is_directory = is_directory;
        entry.playable = is_directory
            ? false
            : is_playable_extension(entry.name, result.normalized_extensions);
        entry.modified_timestamp = parse_http_datetime(modified_text);
        if (!size_text.empty()) {
            try {
                entry.size = static_cast<std::uintmax_t>(std::stoull(size_text));
            } catch (...) {
                entry.size = 0;
            }
        }

        if (entry.is_directory || include_all_entries || entry.playable) {
            result.entries.push_back(std::move(entry));
        }
    }

    report_webdav_browse_progress(progress_callback, WebDavBrowseLoadStage::Finalizing, 0.94f);
    sort_entries(result.entries, general.sort_order);
    result.success = true;
    append_webdav_debug_log(
        "[webdav] browse success entries=" + std::to_string(result.entries.size()) +
        " requested_path=" + result.requested_path);
    return result;
#endif
}

bool send_webdav_delete_request_once(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    bool directory,
    const std::string& authorization_header,
    std::string& error_message) {
#if !SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT
    (void)source;
    (void)relative_path;
    (void)directory;
    (void)authorization_header;
    error_message = "The WebDAV browser backend is not available in this build.";
    return false;
#else
    error_message.clear();
    const std::string normalized_relative_path = webdav_join_relative_path({}, relative_path);
    if (normalized_relative_path.empty()) {
        error_message = "Delete WebDAV file failed: empty path.";
        return false;
    }

    const auto parsed_root_url = parse_http_url(source.url);
    if (!parsed_root_url.has_value()) {
        error_message = "Delete WebDAV file failed: invalid URL.";
        return false;
    }

    const std::vector<std::string> segments = root_and_relative_segments(source, normalized_relative_path);
    const std::string request_url = build_url_from_segments(parsed_root_url.value(), segments, directory);
    const std::string request_target = build_request_target_from_segments(segments, directory);
    append_webdav_debug_log(
        "[webdav][delete] request begin path=" + normalized_relative_path +
        " directory=" + std::string(directory ? "true" : "false") +
        " url=" + sanitize_url_for_debug_log(request_url));

    WebDavConnectionContext connection;
    std::string transport_error;
    if (!connect_webdav_transport(parsed_root_url.value(), connection, transport_error)) {
        error_message = "Delete WebDAV file failed: " + transport_error;
        append_webdav_debug_log("[webdav][delete] connect_failed error=" + error_message);
        return false;
    }

    const std::string request_text =
        build_delete_request_text(parsed_root_url.value(), request_target, authorization_header);
    if (!write_all_to_transport(connection, request_text, transport_error)) {
        error_message = "Delete WebDAV file failed: " + transport_error;
        append_webdav_debug_log("[webdav][delete] write_failed error=" + error_message);
        return false;
    }

    HttpResponse response;
    if (!read_http_response_from_transport(connection, kWebDavMaxResponseBytes, response, transport_error)) {
        error_message = "Delete WebDAV file failed: " + transport_error;
        append_webdav_debug_log("[webdav][delete] response_failed error=" + error_message);
        return false;
    }

    append_webdav_debug_log(
        "[webdav][delete] response status=" + std::to_string(response.status_code) +
        " body_bytes=" + std::to_string(response.body.size()));
    if (response.status_code < 200 || response.status_code >= 300) {
        error_message = "Delete WebDAV file failed: HTTP " + std::to_string(response.status_code);
        append_webdav_debug_log(
            "[webdav][delete] http_error status=" + std::to_string(response.status_code) +
            " body_preview=" + summarize_body_for_debug_log(response.body));
        return false;
    }

    append_webdav_debug_log("[webdav][delete] request success path=" + normalized_relative_path);
    return true;
#endif
}

bool send_webdav_delete_request(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    bool directory,
    std::string& error_message) {
#if !SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT
    (void)source;
    (void)relative_path;
    (void)directory;
    error_message = "The WebDAV browser backend is not available in this build.";
    return false;
#else
    const std::string authorization_header = build_basic_auth_header_for_source(source);
    std::string last_error;
    for (int transport_attempt = 1; transport_attempt <= kWebDavTransientRetryCount; ++transport_attempt) {
        if (send_webdav_delete_request_once(
                source,
                relative_path,
                directory,
                authorization_header,
                last_error)) {
            error_message.clear();
            return true;
        }

        const bool should_retry_transport =
            transport_attempt < kWebDavTransientRetryCount &&
            is_transient_webdav_transport_error(last_error);
        if (!should_retry_transport) {
            break;
        }

        append_webdav_debug_log(
            "[webdav][delete] retry_scheduled attempt=" + std::to_string(transport_attempt) +
            " delay_ms=250");
        svcSleepThread(kWebDavTransientRetryDelayNs);
    }

    error_message = last_error.empty() ? "Delete WebDAV file failed: I/O error" : last_error;
    return false;
#endif
}

bool delete_webdav_directory_recursive(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message);

bool delete_webdav_path_recursive(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message) {
    const std::string normalized_relative_path = webdav_join_relative_path({}, relative_path);
    if (normalized_relative_path.empty()) {
        error_message = "WebDAV file path is empty.";
        return false;
    }

    GeneralSettings general;
    const std::string parent_relative_path = webdav_parent_relative_path(normalized_relative_path);
    const auto parent_result =
        browse_webdav_directory(
            source,
            general,
            parent_relative_path,
            true,
            {});

    if (parent_result.success) {
        const auto found = std::find_if(
            parent_result.entries.begin(),
            parent_result.entries.end(),
            [&normalized_relative_path](const WebDavBrowserEntry& entry) {
                return webdav_join_relative_path({}, entry.relative_path) == normalized_relative_path;
            });
        if (found != parent_result.entries.end()) {
            if (found->is_directory) {
                return delete_webdav_directory_recursive(source, normalized_relative_path, error_message);
            }
            return send_webdav_delete_request(source, normalized_relative_path, false, error_message);
        }
    }

    std::string file_error;
    if (send_webdav_delete_request(source, normalized_relative_path, false, file_error)) {
        error_message.clear();
        return true;
    }

    std::string directory_error;
    if (delete_webdav_directory_recursive(source, normalized_relative_path, directory_error)) {
        error_message.clear();
        return true;
    }

    error_message = file_error;
    if (!directory_error.empty() && directory_error != file_error) {
        if (!error_message.empty()) {
            error_message += " | ";
        }
        error_message += directory_error;
    }
    if (error_message.empty() && !parent_result.success && !parent_result.error_message.empty()) {
        error_message = parent_result.error_message;
    }
    return false;
}

bool delete_webdav_directory_recursive(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message) {
    GeneralSettings general;
    const auto browse_result =
        browse_webdav_directory(
            source,
            general,
            relative_path,
            true,
            {});
    if (!browse_result.success) {
        error_message =
            browse_result.error_message.empty()
                ? "Open WebDAV directory failed: I/O error"
                : browse_result.error_message;
        return false;
    }

    for (const auto& entry : browse_result.entries) {
        if (!delete_webdav_path_recursive(source, entry.relative_path, error_message)) {
            return false;
        }
    }

    return send_webdav_delete_request(source, relative_path, true, error_message);
}

WebDavBrowserResult browse_webdav_directory(
    const WebDavSourceSettings& source,
    const GeneralSettings& general,
    const std::string& relative_path,
    std::function<void(const WebDavBrowseLoadProgress&)> progress_callback) {
    return browse_webdav_directory(source, general, relative_path, false, std::move(progress_callback));
}

WebDavFileProbeResult probe_webdav_file(
    const WebDavSourceSettings& source,
    const std::string& relative_path) {
    WebDavFileProbeResult result;
#if !SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT
    result.backend_available = false;
    result.error_message = "The WebDAV browser backend is not available in this build.";
    return result;
#else
    result.backend_available = true;
    const WebDavFileReadResult read_result =
        fetch_webdav_file_range_once(source, relative_path, 0, 1, build_basic_auth_header_for_source(source));
    result.success = read_result.success;
    result.file_size = read_result.file_size;
    result.error_message = read_result.error_message;
    return result;
#endif
}

WebDavFileReadResult read_webdav_file_range(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::uintmax_t offset,
    std::size_t max_bytes) {
    WebDavFileReadResult result;
#if !SWITCHBOX_HAS_WEBDAV_HTTP_CLIENT
    result.backend_available = false;
    result.error_message = "The WebDAV browser backend is not available in this build.";
    return result;
#else
    return fetch_webdav_file_range_once(
        source,
        relative_path,
        offset,
        max_bytes,
        build_basic_auth_header_for_source(source));
#endif
}

bool delete_webdav_file(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message) {
    return delete_webdav_path_recursive(source, relative_path, error_message);
}

}  // namespace switchbox::core
