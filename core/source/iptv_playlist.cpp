#include "switchbox/core/iptv_playlist.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "switchbox/core/playback_target.hpp"
#include "switchbox/core/switch_mpv_player.hpp"

#if __has_include(<libavformat/avio.h>) && __has_include(<libavformat/avformat.h>) && __has_include(<libavutil/error.h>) && __has_include(<libavutil/opt.h>)
#define SWITCHBOX_HAS_IPTV_FFMPEG_IO 1
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}
#else
#define SWITCHBOX_HAS_IPTV_FFMPEG_IO 0
#endif

namespace switchbox::core {

namespace {

bool is_cancelled(const std::shared_ptr<std::atomic_bool>& cancel_flag) {
    return cancel_flag != nullptr && cancel_flag->load();
}

void report_iptv_playlist_progress(
    const IptvPlaylistProgressCallback& progress_callback,
    float progress,
    IptvPlaylistLoadStage stage,
    std::uint64_t bytes_read = 0,
    std::uint64_t total_bytes = 0) {
    if (!progress_callback) {
        return;
    }

    progress_callback(IptvPlaylistLoadProgress {
        .progress = std::clamp(progress, 0.0f, 1.0f),
        .stage = stage,
        .bytes_read = bytes_read,
        .total_bytes = total_bytes,
    });
}

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string sanitize_text(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        }
    }

    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char character) {
            return (character < 32 && character != ' ') || character == 127;
        }),
        value.end());

    std::string compacted;
    compacted.reserve(value.size());
    bool last_was_space = false;
    for (const unsigned char character : value) {
        const bool is_space = std::isspace(character) != 0;
        if (is_space) {
            if (!last_was_space) {
                compacted.push_back(' ');
            }
            last_was_space = true;
            continue;
        }

        compacted.push_back(static_cast<char>(character));
        last_was_space = false;
    }

    return trim(std::move(compacted));
}

std::string ascii_lower(std::string value) {
    for (char& character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return value;
}

bool is_http_locator(std::string_view locator) {
    std::string lower(locator);
    for (char& character : lower) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (unsigned_character < 0x80) {
            character = static_cast<char>(std::tolower(unsigned_character));
        }
    }
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool has_url_scheme(std::string_view value) {
    if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front()))) {
        return false;
    }

    for (const unsigned char character : value) {
        if (character == ':') {
            return true;
        }
        if (!(std::isalnum(character) != 0 || character == '+' || character == '-' || character == '.')) {
            return false;
        }
    }

    return false;
}

std::string strip_query_and_fragment(std::string_view value) {
    const size_t suffix = value.find_first_of("?#");
    if (suffix == std::string::npos) {
        return std::string(value);
    }

    return std::string(value.substr(0, suffix));
}

std::string extract_url_scheme(std::string_view value) {
    if (!has_url_scheme(value)) {
        return {};
    }

    const size_t separator = value.find(':');
    if (separator == std::string::npos) {
        return {};
    }

    return std::string(value.substr(0, separator));
}

std::string extract_url_origin(std::string_view value) {
    const std::string url = strip_query_and_fragment(value);
    const size_t scheme_separator = url.find("://");
    if (scheme_separator == std::string::npos) {
        return {};
    }

    const size_t path_separator = url.find_first_of("/?#", scheme_separator + 3);
    if (path_separator == std::string::npos) {
        return url;
    }

    return url.substr(0, path_separator);
}

std::string normalize_url_path(std::string_view raw_path) {
    std::vector<std::string> segments;
    std::string current;
    const bool absolute = !raw_path.empty() && raw_path.front() == '/';

    const auto flush = [&segments, &current]() {
        if (current.empty() || current == ".") {
            current.clear();
            return;
        }

        if (current == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
            current.clear();
            return;
        }

        segments.push_back(current);
        current.clear();
    };

    for (const char character : raw_path) {
        if (character == '/') {
            flush();
            continue;
        }
        current.push_back(character);
    }
    flush();

    std::string normalized = absolute ? "/" : "";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            normalized.push_back('/');
        }
        normalized += segments[index];
    }

    if (normalized.empty()) {
        return absolute ? "/" : "";
    }

    return normalized;
}

std::string resolve_playlist_url(std::string_view playlist_url, std::string candidate) {
    candidate = trim(std::move(candidate));
    if (candidate.empty()) {
        return {};
    }

    if (has_url_scheme(candidate)) {
        return candidate;
    }

    const std::string scheme = extract_url_scheme(playlist_url);
    if (!scheme.empty() && candidate.starts_with("//")) {
        return scheme + ":" + candidate;
    }

    const std::string origin = extract_url_origin(playlist_url);
    if (origin.empty()) {
        return candidate;
    }

    const size_t suffix_position = candidate.find_first_of("?#");
    const std::string suffix =
        suffix_position == std::string::npos ? std::string{} : candidate.substr(suffix_position);
    const std::string raw_path =
        suffix_position == std::string::npos ? candidate : candidate.substr(0, suffix_position);

    if (!raw_path.empty() && raw_path.front() == '/') {
        return origin + normalize_url_path(raw_path) + suffix;
    }

    const std::string base_url = strip_query_and_fragment(playlist_url);
    std::string base_path = base_url.substr(origin.size());
    const size_t slash = base_path.find_last_of('/');
    if (slash == std::string::npos) {
        base_path = "/";
    } else {
        base_path = base_path.substr(0, slash + 1);
    }

    return origin + normalize_url_path(base_path + raw_path) + suffix;
}

std::string fallback_title_from_url(std::string_view url) {
    std::string clean_url = strip_query_and_fragment(url);
    const size_t separator = clean_url.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= clean_url.size()) {
        return clean_url;
    }

    return clean_url.substr(separator + 1);
}

std::string extract_quoted_attribute(std::string_view attributes, std::string_view key) {
    const std::string token = std::string(key) + "=\"";
    const size_t begin = attributes.find(token);
    if (begin == std::string::npos) {
        return {};
    }

    const size_t value_begin = begin + token.size();
    const size_t value_end = attributes.find('"', value_begin);
    if (value_end == std::string::npos) {
        return {};
    }

    return sanitize_text(std::string(attributes.substr(value_begin, value_end - value_begin)));
}

std::string normalize_property_key(std::string value) {
    value = ascii_lower(trim(std::move(value)));
    std::replace(value.begin(), value.end(), '_', '-');
    return value;
}

std::string_view trim_view(std::string_view value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

void set_http_header_field(IptvPlaylistEntry& entry, std::string name, std::string value) {
    name = sanitize_text(std::move(name));
    value = sanitize_text(std::move(value));
    if (name.empty() || value.empty()) {
        return;
    }

    const std::string normalized_name = ascii_lower(name);
    if (normalized_name == "user-agent") {
        entry.http_user_agent = std::move(value);
        return;
    }
    if (normalized_name == "referer" || normalized_name == "referrer") {
        entry.http_referrer = std::move(value);
        return;
    }

    const std::string formatted = name + ": " + value;
    for (std::string& existing : entry.http_header_fields) {
        const size_t separator = existing.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string existing_name = ascii_lower(trim(existing.substr(0, separator)));
        if (existing_name == normalized_name) {
            existing = formatted;
            return;
        }
    }

    entry.http_header_fields.push_back(std::move(formatted));
}

void apply_stream_headers_blob(IptvPlaylistEntry& entry, std::string blob) {
    blob = trim(std::move(blob));
    if (blob.empty()) {
        return;
    }

    std::vector<std::string> parts;
    std::string current;
    for (char character : blob) {
        if (character == '&' || character == '|') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(character);
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    if (parts.empty()) {
        parts.push_back(std::move(blob));
    }

    for (std::string& part : parts) {
        part = trim(std::move(part));
        if (part.empty()) {
            continue;
        }

        size_t separator = part.find('=');
        if (separator == std::string::npos) {
            separator = part.find(':');
        }
        if (separator == std::string::npos) {
            continue;
        }

        std::string name = trim(part.substr(0, separator));
        std::string value = trim(part.substr(separator + 1));
        if (name.empty() || value.empty()) {
            continue;
        }

        set_http_header_field(entry, std::move(name), std::move(value));
    }
}

void apply_iptv_property(IptvPlaylistEntry& entry, std::string key, std::string value) {
    const std::string normalized_key = normalize_property_key(std::move(key));
    value = trim(std::move(value));
    if (normalized_key.empty() || value.empty()) {
        return;
    }

    if (normalized_key == "http-user-agent" || normalized_key == "user-agent") {
        entry.http_user_agent = sanitize_text(std::move(value));
        return;
    }

    if (normalized_key == "http-referrer" ||
        normalized_key == "http-referer" ||
        normalized_key == "referrer" ||
        normalized_key == "referer") {
        entry.http_referrer = sanitize_text(std::move(value));
        return;
    }

    if (normalized_key == "http-origin" || normalized_key == "origin") {
        set_http_header_field(entry, "Origin", std::move(value));
        return;
    }

    if (normalized_key == "http-header" ||
        normalized_key == "http-header-fields" ||
        normalized_key == "inputstream.adaptive.stream-headers" ||
        normalized_key == "inputstream.adaptive.manifest-headers" ||
        normalized_key == "inputstream.adaptive.common-headers") {
        apply_stream_headers_blob(entry, std::move(value));
        return;
    }
}

void apply_prefixed_property_line(
    IptvPlaylistEntry& entry,
    std::string_view line,
    std::string_view prefix) {
    if (!line.starts_with(prefix)) {
        return;
    }

    std::string_view payload = trim_view(line.substr(prefix.size()));
    if (payload.empty()) {
        return;
    }

    size_t separator = payload.find('=');
    if (separator == std::string::npos) {
        separator = payload.find(':');
    }
    if (separator == std::string::npos) {
        return;
    }

    std::string key = std::string(trim_view(payload.substr(0, separator)));
    std::string value = std::string(trim_view(payload.substr(separator + 1)));
    if (key.empty() || value.empty()) {
        return;
    }

    apply_iptv_property(entry, std::move(key), std::move(value));
}

uint64_t fnv1a64(std::string_view value) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }

    return hash;
}

std::string to_hex(uint64_t value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(16, '0');

    for (int index = 15; index >= 0; --index) {
        result[static_cast<size_t>(index)] = hex[value & 0x0F];
        value >>= 4;
    }

    return result;
}

std::string make_iptv_entry_favorite_key(const IptvPlaylistEntry& entry) {
    std::string identity;

    if (!entry.tvg_id.empty()) {
        identity += "id:";
        identity += entry.tvg_id;
    } else if (!entry.tvg_name.empty()) {
        identity += "name:";
        identity += entry.tvg_name;
    } else if (!entry.title.empty()) {
        identity += "title:";
        identity += entry.title;
    }

    if (!entry.group_title.empty()) {
        if (!identity.empty()) {
            identity += '|';
        }
        identity += "group:";
        identity += entry.group_title;
    }

    const std::string clean_url = strip_query_and_fragment(entry.stream_url);
    if (!clean_url.empty()) {
        if (!identity.empty()) {
            identity += '|';
        }
        identity += "url:";
        identity += clean_url;
    }

    if (identity.empty()) {
        identity = "fallback";
    }

    return "iptv_" + to_hex(fnv1a64(identity));
}

struct IptvHttpOptions {
    std::string user_agent;
    std::string referrer;
    std::vector<std::string> header_fields;
};

struct IptvPrefixProbeResult {
    std::string data;
    std::string error_message;
    std::string resolved_url;
    std::string mime_type;
    bool hit_read_limit = false;
    bool looks_like_text = true;
};

std::string build_default_iptv_user_agent() {
    // Match the proven UA used by playback open so any manifest prefetch in the
    // prepare phase sees the same server behavior as libmpv itself.
    return "Mozilla/5.0 (X11; Linux x86_64; rv:49.0) Gecko/20100101 Firefox/49.0";
}

std::string normalize_header_name(std::string value) {
    value = ascii_lower(trim(std::move(value)));
    std::replace(value.begin(), value.end(), '_', '-');
    return value;
}

bool has_header_named(const std::vector<std::string>& fields, std::string_view name) {
    const std::string expected = normalize_header_name(std::string(name));
    for (const std::string& field : fields) {
        const size_t separator = field.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        if (normalize_header_name(field.substr(0, separator)) == expected) {
            return true;
        }
    }

    return false;
}

bool locator_looks_like_hls_playlist(std::string_view locator) {
    std::string lower = ascii_lower(strip_query_and_fragment(locator));
    return lower.size() >= 5 && lower.ends_with(".m3u8");
}

bool looks_like_probe_sensitive_direct_locator(std::string_view locator) {
    if (!is_http_locator(locator) || locator_looks_like_hls_playlist(locator)) {
        return false;
    }

    const std::string lower = ascii_lower(strip_query_and_fragment(locator));
    return lower.find("live.metshop.top/huya/") != std::string::npos;
}

std::string rewrite_probe_sensitive_locator_for_playback(std::string locator) {
    const std::string lower = ascii_lower(locator);
    constexpr std::string_view https_prefix = "https://live.metshop.top/huya/";
    if (lower.rfind(https_prefix, 0) == 0) {
        locator.replace(0, 5, "http");
    }
    return locator;
}

bool looks_like_mpeg_ts_payload(std::string_view bytes) {
    if (bytes.size() < 188 * 3) {
        return false;
    }

    const size_t max_offset = std::min<size_t>(188, bytes.size() - (188 * 2));
    for (size_t offset = 0; offset < max_offset; ++offset) {
        if (static_cast<unsigned char>(bytes[offset]) == 0x47 &&
            static_cast<unsigned char>(bytes[offset + 188]) == 0x47 &&
            static_cast<unsigned char>(bytes[offset + 376]) == 0x47) {
            return true;
        }
    }

    return false;
}

bool looks_like_flv_payload(std::string_view bytes) {
    return bytes.size() >= 3 &&
           bytes[0] == 'F' &&
           bytes[1] == 'L' &&
           bytes[2] == 'V';
}

bool looks_like_text_payload(std::string_view bytes) {
    if (bytes.empty()) {
        return true;
    }

    size_t control_count = 0;
    for (const unsigned char byte : bytes) {
        if (byte == 0) {
            return false;
        }

        if (byte < 0x20 && byte != '\r' && byte != '\n' && byte != '\t') {
            ++control_count;
        }
    }

    return control_count * 32 <= bytes.size();
}

IptvPreparedStreamClass guess_direct_stream_class(
    std::string_view locator,
    std::string_view prefix_bytes) {
    const std::string lower_locator = ascii_lower(strip_query_and_fragment(locator));
    if (lower_locator.ends_with(".flv") || looks_like_flv_payload(prefix_bytes)) {
        return IptvPreparedStreamClass::DirectFlv;
    }

    if (lower_locator.find("mpegts") != std::string::npos ||
        lower_locator.ends_with(".ts") ||
        looks_like_mpeg_ts_payload(prefix_bytes)) {
        return IptvPreparedStreamClass::DirectTs;
    }

    if (locator_looks_like_hls_playlist(locator)) {
        return IptvPreparedStreamClass::MediaHlsLive;
    }

    return IptvPreparedStreamClass::DirectHttp;
}

std::string join_http_headers_for_ffmpeg(const std::vector<std::string>& fields) {
    std::string result;
    for (const std::string& field : fields) {
        if (trim(field).empty()) {
            continue;
        }

        result += trim(field);
        if (result.size() < 2 || result.substr(result.size() - 2) != "\r\n") {
            result += "\r\n";
        }
    }
    return result;
}

void finalize_playlist_entry(IptvPlaylistEntry& entry) {
    entry.title = sanitize_text(std::move(entry.title));
    entry.group_title = sanitize_text(std::move(entry.group_title));
    entry.tvg_name = sanitize_text(std::move(entry.tvg_name));
    entry.tvg_id = sanitize_text(std::move(entry.tvg_id));
    entry.tvg_country = sanitize_text(std::move(entry.tvg_country));
    entry.tvg_chno = sanitize_text(std::move(entry.tvg_chno));
    entry.http_user_agent = sanitize_text(std::move(entry.http_user_agent));
    entry.http_referrer = sanitize_text(std::move(entry.http_referrer));
    for (std::string& header : entry.http_header_fields) {
        header = sanitize_text(std::move(header));
    }
    entry.http_header_fields.erase(
        std::remove_if(
            entry.http_header_fields.begin(),
            entry.http_header_fields.end(),
            [](const std::string& header) {
                return header.empty();
            }),
        entry.http_header_fields.end());
    entry.favorite_key = make_iptv_entry_favorite_key(entry);

    // These fields are currently not used after favorite-key generation and
    // keeping them increases memory pressure noticeably on very large playlists.
    entry.logo_url.clear();
    entry.tvg_id.clear();
    entry.tvg_name.clear();
}

IptvPlaylistEntry parse_extinf_line(std::string_view line) {
    IptvPlaylistEntry entry;
    constexpr std::string_view prefix = "#EXTINF:";
    if (!line.starts_with(prefix)) {
        return entry;
    }

    const std::string_view payload = line.substr(prefix.size());
    const size_t comma = payload.find(',');
    const std::string_view attributes = comma == std::string::npos ? payload : payload.substr(0, comma);

    entry.tvg_id = extract_quoted_attribute(attributes, "tvg-id");
    entry.tvg_name = extract_quoted_attribute(attributes, "tvg-name");
    entry.tvg_country = extract_quoted_attribute(attributes, "tvg-country");
    entry.tvg_chno = extract_quoted_attribute(attributes, "tvg-chno");
    entry.logo_url = extract_quoted_attribute(attributes, "tvg-logo");
    entry.group_title = extract_quoted_attribute(attributes, "group-title");

    if (comma != std::string::npos && comma + 1 < payload.size()) {
        entry.title = sanitize_text(std::string(payload.substr(comma + 1)));
    }

    if (entry.title.empty()) {
        entry.title = entry.tvg_name;
    }

    return entry;
}

bool looks_like_hls_playlist(std::string_view text) {
    return text.find("#EXT-X-TARGETDURATION") != std::string::npos ||
           text.find("#EXT-X-STREAM-INF") != std::string::npos ||
           text.find("#EXT-X-MEDIA-SEQUENCE") != std::string::npos ||
           text.find("#EXT-X-VERSION") != std::string::npos;
}

bool is_static_hls_playlist(std::string_view text) {
    const std::string lower = ascii_lower(std::string(text));
    return lower.find("#ext-x-endlist") != std::string::npos ||
           lower.find("#ext-x-playlist-type:vod") != std::string::npos;
}

int parse_hls_bandwidth(std::string_view line) {
    constexpr std::string_view token = "BANDWIDTH=";
    const size_t token_position = line.find(token);
    if (token_position == std::string::npos) {
        return -1;
    }

    size_t value_begin = token_position + token.size();
    size_t value_end = value_begin;
    while (value_end < line.size() && std::isdigit(static_cast<unsigned char>(line[value_end])) != 0) {
        ++value_end;
    }

    if (value_end == value_begin) {
        return -1;
    }

    try {
        return std::stoi(std::string(line.substr(value_begin, value_end - value_begin)));
    } catch (...) {
        return -1;
    }
}

std::string resolve_hls_master_variant_url(const std::string& playlist_url, std::string_view text) {
    std::istringstream input{std::string(text)};
    std::string line;
    bool waiting_for_variant_url = false;
    int pending_bandwidth = -1;
    static constexpr int kPreferredBandwidthCap = 2500000;
    int best_below_cap_bandwidth = -1;
    int lowest_above_cap_bandwidth = -1;
    std::string best_url;
    std::string fallback_url;

    while (std::getline(input, line)) {
        if (!line.empty() && static_cast<unsigned char>(line.front()) == 0xEF) {
            static constexpr std::array<unsigned char, 3> utf8_bom = {0xEF, 0xBB, 0xBF};
            if (line.size() >= utf8_bom.size() &&
                static_cast<unsigned char>(line[0]) == utf8_bom[0] &&
                static_cast<unsigned char>(line[1]) == utf8_bom[1] &&
                static_cast<unsigned char>(line[2]) == utf8_bom[2]) {
                line.erase(0, utf8_bom.size());
            }
        }

        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        if (line.starts_with("#EXT-X-STREAM-INF:")) {
            waiting_for_variant_url = true;
            pending_bandwidth = parse_hls_bandwidth(line);
            continue;
        }

        if (line.front() == '#') {
            continue;
        }

        if (!waiting_for_variant_url) {
            continue;
        }

        waiting_for_variant_url = false;
        const std::string candidate_url = resolve_playlist_url(playlist_url, line);
        if (candidate_url.empty()) {
            continue;
        }

        if (pending_bandwidth > 0 && pending_bandwidth <= kPreferredBandwidthCap) {
            if (best_url.empty() || pending_bandwidth > best_below_cap_bandwidth) {
                best_url = candidate_url;
                best_below_cap_bandwidth = pending_bandwidth;
            }
            continue;
        }

        if (fallback_url.empty() ||
            lowest_above_cap_bandwidth < 0 ||
            (pending_bandwidth > 0 && pending_bandwidth < lowest_above_cap_bandwidth)) {
            fallback_url = candidate_url;
            lowest_above_cap_bandwidth = pending_bandwidth;
        }

        if (best_url.empty() && fallback_url.empty()) {
            best_url = candidate_url;
        }
    }

    return !best_url.empty() ? best_url : fallback_url;
}

IptvHttpOptions build_playback_http_options(const PlaybackTarget& target) {
    IptvHttpOptions options;
    options.user_agent =
        trim(target.http_user_agent).empty() ? build_default_iptv_user_agent() : trim(target.http_user_agent);
    options.referrer = trim(target.http_referrer);
    options.header_fields = target.http_header_fields;
    return options;
}

std::string join_http_header_fields_for_mpv(const std::vector<std::string>& fields) {
    std::string result;
    for (const std::string& field : fields) {
        if (trim(field).empty()) {
            continue;
        }

        if (!result.empty()) {
            result += ",";
        }
        result += trim(field);
    }
    return result;
}

void set_open_option_pair(
    std::vector<std::pair<std::string, std::string>>& options,
    std::string key,
    std::string value) {
    key = trim(std::move(key));
    value = trim(std::move(value));
    if (key.empty() || value.empty()) {
        return;
    }

    for (auto& option : options) {
        if (option.first == key) {
            option.second = std::move(value);
            return;
        }
    }

    options.emplace_back(std::move(key), std::move(value));
}

void append_lavf_key_value(std::string& destination, std::string_view key, std::string value) {
    value = trim(std::move(value));
    if (value.empty()) {
        return;
    }

    if (value.find(',') != std::string::npos ||
        value.find('\r') != std::string::npos ||
        value.find('\n') != std::string::npos) {
        return;
    }

    if (!destination.empty()) {
        destination += ",";
    }
    destination += std::string(key);
    destination += "=";
    destination += value;
}

std::string stream_class_name(IptvPreparedStreamClass stream_class) {
    switch (stream_class) {
        case IptvPreparedStreamClass::DirectHttp:
            return "direct_http";
        case IptvPreparedStreamClass::DirectFlv:
            return "direct_flv";
        case IptvPreparedStreamClass::MasterHls:
            return "master_hls";
        case IptvPreparedStreamClass::MediaHlsLive:
            return "media_hls_live";
        case IptvPreparedStreamClass::MediaHlsVod:
            return "media_hls_vod";
        case IptvPreparedStreamClass::DirectTs:
            return "direct_ts";
        case IptvPreparedStreamClass::Unsupported:
            return "unsupported";
        case IptvPreparedStreamClass::Unknown:
        default:
            return "unknown";
    }
}

bool seems_like_html_or_proxy_text(std::string_view text) {
    const std::string lower = ascii_lower(std::string(text));
    return lower.find("<!doctype html") != std::string::npos ||
           lower.find("<html") != std::string::npos ||
           lower.find("<script") != std::string::npos ||
           lower.find("window.location") != std::string::npos ||
           lower.find("document.") != std::string::npos;
}

std::vector<std::pair<std::string, std::string>> build_open_options_for_class(
    IptvPreparedStreamClass stream_class,
    const IptvHttpOptions& http_options) {
    std::vector<std::pair<std::string, std::string>> options;
    const std::string user_agent =
        trim(http_options.user_agent).empty() ? build_default_iptv_user_agent() : trim(http_options.user_agent);
    const std::string referrer = trim(http_options.referrer);

    std::string stream_lavf_options;
    std::string demuxer_lavf_options;

    if (stream_class == IptvPreparedStreamClass::MediaHlsVod) {
        // Some IPTV VOD sources serve valid HLS content but break on HEAD or
        // seek probes. Treat the stream as non-seekable so lavf goes directly
        // to plain GET requests for the playlist and media segments.
        stream_lavf_options =
            "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=3,"
            "multiple_requests=0,tls_verify=0,seekable=0";
        demuxer_lavf_options = "http_persistent=0,http_multiple=0,tls_verify=0,seekable=0";
        append_lavf_key_value(demuxer_lavf_options, "user_agent", user_agent);
        append_lavf_key_value(demuxer_lavf_options, "referer", referrer);
    } else if (stream_class == IptvPreparedStreamClass::DirectFlv) {
        // Some direct FLV live sources reject reconnect/multi-request probing
        // patterns after the first chunk. Use a single plain HTTP session and
        // force the demuxer format so FILE_LOADED does not wait on autodetect.
        stream_lavf_options =
            "reconnect=0,reconnect_streamed=0,reconnect_on_network_error=0,reconnect_delay_max=0,"
            "multiple_requests=0,tls_verify=0,seekable=0";
        demuxer_lavf_options = "http_persistent=0,http_multiple=0,tls_verify=0,seekable=0";
        append_lavf_key_value(demuxer_lavf_options, "user_agent", user_agent);
        append_lavf_key_value(demuxer_lavf_options, "referer", referrer);
    } else if (stream_class == IptvPreparedStreamClass::DirectTs) {
        stream_lavf_options =
            "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_delay_max=3,"
            "multiple_requests=1,tls_verify=0";
        demuxer_lavf_options = "http_persistent=1,http_multiple=1,tls_verify=0";
        append_lavf_key_value(demuxer_lavf_options, "user_agent", user_agent);
        append_lavf_key_value(demuxer_lavf_options, "referer", referrer);
    }

    set_open_option_pair(options, "user-agent", user_agent);
    set_open_option_pair(options, "tls-verify", "no");
    set_open_option_pair(options, "network-timeout", "45");

    if (!referrer.empty()) {
        set_open_option_pair(options, "referrer", referrer);
    }

    const std::string joined_headers = join_http_header_fields_for_mpv(http_options.header_fields);
    if (!joined_headers.empty()) {
        set_open_option_pair(options, "http-header-fields", joined_headers);
    }

    if (!stream_lavf_options.empty()) {
        set_open_option_pair(options, "stream-lavf-o", stream_lavf_options);
    }

    if (stream_class == IptvPreparedStreamClass::DirectFlv) {
        set_open_option_pair(options, "demuxer", "+lavf");
        set_open_option_pair(options, "demuxer-lavf-format", "flv");
    } else if (stream_class == IptvPreparedStreamClass::DirectTs) {
        set_open_option_pair(options, "demuxer", "+lavf");
        set_open_option_pair(options, "demuxer-lavf-format", "mpegts");
    }

    if (!demuxer_lavf_options.empty()) {
        set_open_option_pair(options, "demuxer-lavf-o", demuxer_lavf_options);
    }
    return options;
}

std::vector<IptvPlaylistEntry> parse_iptv_playlist_text(
    const std::string& playlist_url,
    std::string_view text) {
    std::vector<IptvPlaylistEntry> entries;
    std::istringstream input{std::string(text)};
    std::string line;
    IptvPlaylistEntry pending_entry;
    bool waiting_for_url = false;

    while (std::getline(input, line)) {
        if (!line.empty() && static_cast<unsigned char>(line.front()) == 0xEF) {
            static constexpr std::array<unsigned char, 3> utf8_bom = {0xEF, 0xBB, 0xBF};
            if (line.size() >= utf8_bom.size() &&
                static_cast<unsigned char>(line[0]) == utf8_bom[0] &&
                static_cast<unsigned char>(line[1]) == utf8_bom[1] &&
                static_cast<unsigned char>(line[2]) == utf8_bom[2]) {
                line.erase(0, utf8_bom.size());
            }
        }

        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }

        if (line.starts_with("#EXTINF:")) {
            pending_entry = parse_extinf_line(line);
            waiting_for_url = true;
            continue;
        }

        if (line.starts_with("#EXTGRP:") && waiting_for_url && pending_entry.group_title.empty()) {
            pending_entry.group_title = sanitize_text(line.substr(8));
            continue;
        }

        if (waiting_for_url) {
            if (line.starts_with("#EXTVLCOPT:")) {
                apply_prefixed_property_line(pending_entry, line, "#EXTVLCOPT:");
                continue;
            }
            if (line.starts_with("#KODIPROP:")) {
                apply_prefixed_property_line(pending_entry, line, "#KODIPROP:");
                continue;
            }
        }

        if (line.front() == '#') {
            continue;
        }

        if (!waiting_for_url) {
            pending_entry = {};
            pending_entry.title = fallback_title_from_url(line);
        }

        pending_entry.stream_url = resolve_playlist_url(playlist_url, line);
        if (pending_entry.title.empty()) {
            pending_entry.title = fallback_title_from_url(pending_entry.stream_url);
        }
        if (pending_entry.title.empty()) {
            pending_entry.title = pending_entry.tvg_name;
        }

        if (!pending_entry.stream_url.empty()) {
            finalize_playlist_entry(pending_entry);
            entries.push_back(std::move(pending_entry));
        }

        pending_entry = {};
        waiting_for_url = false;
    }

    return entries;
}

#if SWITCHBOX_HAS_IPTV_FFMPEG_IO
std::string get_avio_child_string_option(AVIOContext* io_context, const char* option_name) {
    if (io_context == nullptr || option_name == nullptr) {
        return {};
    }

    uint8_t* raw_value = nullptr;
    const int rc = av_opt_get(io_context, option_name, AV_OPT_SEARCH_CHILDREN, &raw_value);
    if (rc < 0 || raw_value == nullptr) {
        return {};
    }

    std::string value(reinterpret_cast<char*>(raw_value));
    av_free(raw_value);
    return trim(std::move(value));
}

std::string resolve_effective_http_url_from_avio(
    const std::string& original_url,
    AVIOContext* io_context) {
    std::string candidate = get_avio_child_string_option(io_context, "location");
    if (candidate.empty()) {
        candidate = get_avio_child_string_option(io_context, "resource");
    }

    candidate = trim(std::move(candidate));
    if (candidate.empty()) {
        return {};
    }

    if (is_http_locator(candidate)) {
        return candidate;
    }

    if (!candidate.empty() && (candidate.front() == '/' || !has_url_scheme(candidate))) {
        return resolve_playlist_url(original_url, candidate);
    }

    return {};
}

std::string av_error_to_string(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer {};
    av_strerror(error_code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

struct IptvInterruptContext {
    const std::shared_ptr<std::atomic_bool>* cancel_flag = nullptr;
};

int interrupt_iptv_io(void* opaque) {
    auto* context = static_cast<IptvInterruptContext*>(opaque);
    if (context == nullptr || context->cancel_flag == nullptr) {
        return 0;
    }

    return is_cancelled(*context->cancel_flag) ? 1 : 0;
}

void apply_ffmpeg_http_options(
    AVDictionary** options,
    const IptvHttpOptions& http_options,
    std::string_view timeout_us) {
    const std::string user_agent =
        trim(http_options.user_agent).empty() ? build_default_iptv_user_agent() : trim(http_options.user_agent);
    const std::string referrer = trim(http_options.referrer);

    std::vector<std::string> header_fields = http_options.header_fields;
    if (!has_header_named(header_fields, "connection")) {
        header_fields.emplace_back("Connection: close");
    }

    av_dict_set(options, "user_agent", user_agent.c_str(), 0);
    av_dict_set(options, "timeout", std::string(timeout_us).c_str(), 0);
    av_dict_set(options, "rw_timeout", std::string(timeout_us).c_str(), 0);
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_streamed", "1", 0);
    av_dict_set(options, "reconnect_on_network_error", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "3", 0);
    av_dict_set(options, "multiple_requests", "0", 0);
    av_dict_set(options, "http_persistent", "0", 0);
    av_dict_set(options, "seekable", "0", 0);
    av_dict_set(options, "tls_verify", "0", 0);

    if (!referrer.empty()) {
        av_dict_set(options, "referer", referrer.c_str(), 0);
    }

    const std::string headers = join_http_headers_for_ffmpeg(header_fields);
    if (!headers.empty()) {
        av_dict_set(options, "headers", headers.c_str(), 0);
    }
}

std::string fetch_bytes_via_ffmpeg_once(
    const std::string& url,
    const IptvHttpOptions& http_options,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    std::string& error_message,
    const IptvPlaylistProgressCallback& progress_callback = {},
    std::string_view timeout_us = "30000000",
    size_t max_bytes = 0,
    bool* hit_read_limit = nullptr,
    std::string* resolved_url = nullptr,
    std::string* mime_type = nullptr) {
    error_message.clear();
    if (hit_read_limit != nullptr) {
        *hit_read_limit = false;
    }
    if (resolved_url != nullptr) {
        resolved_url->clear();
    }
    if (mime_type != nullptr) {
        mime_type->clear();
    }
    avformat_network_init();

    AVDictionary* options = nullptr;
    AVIOContext* io_context = nullptr;
    apply_ffmpeg_http_options(&options, http_options, timeout_us);

    IptvInterruptContext interrupt_context {&cancel_flag};
    AVIOInterruptCB interrupt_callback {
        .callback = interrupt_iptv_io,
        .opaque = &interrupt_context,
    };

    const int open_result =
        avio_open2(&io_context, url.c_str(), AVIO_FLAG_READ, &interrupt_callback, &options);
    av_dict_free(&options);
    if (open_result < 0) {
        if (is_cancelled(cancel_flag) || open_result == AVERROR_EXIT) {
            error_message = "Cancelled";
        } else {
            error_message = "Open playlist failed: " + av_error_to_string(open_result);
        }
        return {};
    }

    if (resolved_url != nullptr) {
        *resolved_url = resolve_effective_http_url_from_avio(url, io_context);
    }
    if (mime_type != nullptr) {
        *mime_type = get_avio_child_string_option(io_context, "mime_type");
    }

    std::string text;
    const int64_t size = avio_size(io_context);
    const std::uint64_t total_bytes = size > 0 ? static_cast<std::uint64_t>(size) : 0;
    std::uint64_t bytes_read = 0;
    if (size > 0 && size <= 8 * 1024 * 1024) {
        text.reserve(static_cast<size_t>(size));
    }

    report_iptv_playlist_progress(
        progress_callback,
        total_bytes > 0 ? 0.12f : 0.15f,
        IptvPlaylistLoadStage::DownloadingPlaylist,
        0,
        total_bytes);

    // Keep the read buffer on the heap to avoid stressing the async worker
    // thread stack on Switch when fetching large remote playlists.
    auto buffer = std::make_unique<std::vector<unsigned char>>(8 * 1024);
    while (true) {
        if (is_cancelled(cancel_flag)) {
            error_message = "Cancelled";
            avio_closep(&io_context);
            return {};
        }

        const int read_result =
            avio_read(io_context, buffer->data(), static_cast<int>(buffer->size()));
        if (read_result == AVERROR_EOF || read_result == 0) {
            break;
        }

        if (read_result < 0) {
            if (is_cancelled(cancel_flag) || read_result == AVERROR_EXIT) {
                error_message = "Cancelled";
            } else {
                error_message = "Read playlist failed: " + av_error_to_string(read_result);
            }
            avio_closep(&io_context);
            return {};
        }

        size_t to_append = static_cast<size_t>(read_result);
        if (max_bytes > 0 && text.size() + to_append > max_bytes) {
            to_append = max_bytes - text.size();
            if (hit_read_limit != nullptr) {
                *hit_read_limit = true;
            }
        }

        if (to_append > 0) {
            text.append(reinterpret_cast<const char*>(buffer->data()), to_append);
            bytes_read += static_cast<std::uint64_t>(to_append);
            float download_progress = 0.15f;
            if (total_bytes > 0) {
                const double ratio = std::clamp(
                    static_cast<double>(bytes_read) / static_cast<double>(total_bytes),
                    0.0,
                    1.0);
                download_progress = 0.12f + static_cast<float>(ratio) * 0.58f;
            } else if (bytes_read > 0) {
                const double ratio = std::clamp(
                    static_cast<double>(bytes_read) / (512.0 * 1024.0),
                    0.0,
                    1.0);
                download_progress = 0.15f + static_cast<float>(ratio) * 0.45f;
            }
            report_iptv_playlist_progress(
                progress_callback,
                download_progress,
                IptvPlaylistLoadStage::DownloadingPlaylist,
                bytes_read,
                total_bytes);
        }

        if (max_bytes > 0 && text.size() >= max_bytes) {
            break;
        }
    }

    avio_closep(&io_context);
    return text;
}

std::string fetch_text_via_ffmpeg_once(
    const std::string& url,
    const IptvHttpOptions& http_options,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    std::string& error_message,
    const IptvPlaylistProgressCallback& progress_callback = {},
    std::string_view timeout_us = "30000000") {
    bool ignored_hit_read_limit = false;
    return fetch_bytes_via_ffmpeg_once(
        url,
        http_options,
        cancel_flag,
        error_message,
        progress_callback,
        timeout_us,
        0,
        &ignored_hit_read_limit);
}

std::string fetch_text_via_ffmpeg(
    const std::string& url,
    const IptvHttpOptions& http_options,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    std::string& error_message,
    const IptvPlaylistProgressCallback& progress_callback = {}) {
    static constexpr int kMaxAttempts = 3;
    std::string last_error;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (is_cancelled(cancel_flag)) {
            error_message = "Cancelled";
            return {};
        }

        std::string text = fetch_text_via_ffmpeg_once(
            url,
            http_options,
            cancel_flag,
            last_error,
            progress_callback);
        if (!last_error.empty()) {
            if (is_cancelled(cancel_flag) || last_error == "Cancelled") {
                error_message = "Cancelled";
                return {};
            }

            if (attempt < kMaxAttempts) {
                for (int step = 0; step < attempt * 3; ++step) {
                    if (is_cancelled(cancel_flag)) {
                        error_message = "Cancelled";
                        return {};
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            error_message = last_error + " (attempt " + std::to_string(attempt) + "/" +
                            std::to_string(kMaxAttempts) + ")";
            return {};
        }

        error_message.clear();
        return text;
    }

    error_message = last_error;
    return {};
}

IptvPrefixProbeResult probe_prefix_via_ffmpeg(
    const std::string& url,
    const IptvHttpOptions& http_options,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    size_t max_bytes = 256 * 1024) {
    static constexpr int kMaxAttempts = 3;

    IptvPrefixProbeResult result;
    std::string last_error;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (is_cancelled(cancel_flag)) {
            result.error_message = "Cancelled";
            return result;
        }

        bool hit_read_limit = false;
        std::string resolved_url;
        std::string mime_type;
        std::string data = fetch_bytes_via_ffmpeg_once(
            url,
            http_options,
            cancel_flag,
            last_error,
            {},
            "15000000",
            max_bytes,
            &hit_read_limit,
            &resolved_url,
            &mime_type);
        if (!last_error.empty()) {
            if (is_cancelled(cancel_flag) || last_error == "Cancelled") {
                result.error_message = "Cancelled";
                return result;
            }

            if (attempt < kMaxAttempts) {
                for (int step = 0; step < attempt * 3; ++step) {
                    if (is_cancelled(cancel_flag)) {
                        result.error_message = "Cancelled";
                        return result;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            result.error_message = last_error + " (attempt " + std::to_string(attempt) + "/" +
                                   std::to_string(kMaxAttempts) + ")";
            return result;
        }

        result.data = std::move(data);
        result.resolved_url = std::move(resolved_url);
        result.mime_type = std::move(mime_type);
        result.hit_read_limit = hit_read_limit;
        result.looks_like_text = looks_like_text_payload(result.data);
        return result;
    }

    result.error_message = last_error;
    return result;
}
#endif

}  // namespace

IptvPlaylistResult load_iptv_playlist(
    const IptvSourceSettings& source,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    IptvPlaylistProgressCallback progress_callback) {
    IptvPlaylistResult result;
#if SWITCHBOX_HAS_IPTV_FFMPEG_IO
    result.backend_available = true;
    report_iptv_playlist_progress(progress_callback, 0.02f, IptvPlaylistLoadStage::Starting);

    if (trim(source.url).empty()) {
        result.error_message = "Playlist URL is not configured.";
        return result;
    }

    std::string error_message;
    report_iptv_playlist_progress(progress_callback, 0.08f, IptvPlaylistLoadStage::OpeningConnection);
    const std::string playlist_text =
        fetch_text_via_ffmpeg(source.url, {}, cancel_flag, error_message, progress_callback);
    if (error_message == "Cancelled") {
        result.error_message.clear();
        return result;
    }

    if (!error_message.empty()) {
        result.error_message = std::move(error_message);
        return result;
    }

    if (playlist_text.empty()) {
        result.error_message = "Playlist is empty.";
        return result;
    }

    report_iptv_playlist_progress(progress_callback, 0.80f, IptvPlaylistLoadStage::ParsingPlaylist);
    if (looks_like_hls_playlist(playlist_text)) {
        IptvPlaylistEntry entry;
        entry.title = source.title.empty() ? fallback_title_from_url(source.url) : source.title;
        entry.stream_url = source.url;
        finalize_playlist_entry(entry);
        result.entries.push_back(std::move(entry));
        result.success = true;
        report_iptv_playlist_progress(progress_callback, 1.0f, IptvPlaylistLoadStage::Finalizing);
        return result;
    }

    result.entries = parse_iptv_playlist_text(source.url, playlist_text);
    if (result.entries.empty()) {
        result.error_message = "Playlist does not contain any playable channels.";
        return result;
    }

    report_iptv_playlist_progress(progress_callback, 0.96f, IptvPlaylistLoadStage::Finalizing);
    result.success = true;
    report_iptv_playlist_progress(progress_callback, 1.0f, IptvPlaylistLoadStage::Finalizing);
    return result;
#else
    (void)source;
    (void)progress_callback;
    result.backend_available = false;
    result.error_message = "IPTV playlist backend is not available in this build.";
    return result;
#endif
}

IptvOpenPlan prepare_iptv_open_plan_for_playback(
    const PlaybackTarget& target,
    const std::shared_ptr<std::atomic_bool>& cancel_flag,
    IptvOpenPlanProgressCallback progress_callback) {
    IptvOpenPlan plan;
    const auto report_progress = [&](IptvOpenPlanStage stage, float progress) {
        if (!progress_callback) {
            return;
        }

        progress_callback({
            .progress = std::clamp(progress, 0.0f, 1.0f),
            .stage = stage,
        });
    };

    report_progress(IptvOpenPlanStage::Starting, 0.02f);
    std::string locator =
        trim(target.primary_locator.empty() ? target.fallback_locator : target.primary_locator);
    report_progress(IptvOpenPlanStage::NormalizingLocator, 0.08f);
    if (looks_like_probe_sensitive_direct_locator(locator)) {
        const std::string rewritten_locator = rewrite_probe_sensitive_locator_for_playback(locator);
        if (rewritten_locator != locator) {
            switchbox::core::switch_mpv_append_debug_log_note(
                "[iptv-prepare] locator_rewrite from=" + locator + " to=" + rewritten_locator);
            locator = std::move(rewritten_locator);
        }
    }
    plan.final_locator = locator;

    const IptvHttpOptions http_options = build_playback_http_options(target);
    plan.effective_user_agent = http_options.user_agent;
    plan.effective_referrer = http_options.referrer;
    plan.effective_header_fields = http_options.header_fields;

    const auto apply_class_tuning = [&](IptvPreparedStreamClass stream_class) {
        plan.analyzeduration_override.clear();
        plan.probescore_override.clear();

        switch (stream_class) {
            case IptvPreparedStreamClass::DirectHttp:
                plan.analyzeduration_override = "1.2";
                plan.probescore_override = "48";
                break;
            case IptvPreparedStreamClass::DirectFlv:
                plan.analyzeduration_override = "1.2";
                plan.probescore_override = "48";
                break;
            case IptvPreparedStreamClass::DirectTs:
                plan.analyzeduration_override = "0.6";
                plan.probescore_override = "28";
                break;
            case IptvPreparedStreamClass::MasterHls:
            case IptvPreparedStreamClass::MediaHlsVod:
                plan.analyzeduration_override = "0.6";
                plan.probescore_override = "28";
                break;
            case IptvPreparedStreamClass::MediaHlsLive:
            case IptvPreparedStreamClass::Unsupported:
            case IptvPreparedStreamClass::Unknown:
            default:
                break;
        }
    };

    const auto apply_class_defaults = [&](IptvPreparedStreamClass stream_class, std::string reason) {
        plan.stream_class = stream_class;
        plan.option_pairs = build_open_options_for_class(stream_class, http_options);
        apply_class_tuning(stream_class);
        plan.debug_summary =
            "class=" + stream_class_name(stream_class) + (reason.empty() ? std::string() : " " + std::move(reason));
    };

    if (locator.empty() || !is_http_locator(locator)) {
        report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
        apply_class_defaults(IptvPreparedStreamClass::Unknown, "non_http_locator");
        return plan;
    }

    if (looks_like_probe_sensitive_direct_locator(locator)) {
        report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
        const IptvPreparedStreamClass direct_class = IptvPreparedStreamClass::DirectFlv;
        switchbox::core::switch_mpv_append_debug_log_note(
            "[iptv-prepare] probe_skipped locator=" + locator + " reason=sensitive_direct_locator");
        apply_class_defaults(direct_class, "probe_skipped_sensitive_direct_flv");
        return plan;
    }

#if SWITCHBOX_HAS_IPTV_FFMPEG_IO
    report_progress(IptvOpenPlanStage::ProbingSource, 0.18f);
    const auto initial_probe = probe_prefix_via_ffmpeg(locator, http_options, cancel_flag);
    if (initial_probe.error_message == "Cancelled") {
        plan.success = false;
        plan.user_visible_reason = "Cancelled";
        plan.debug_summary = "cancelled";
        return plan;
    }

    if (!initial_probe.error_message.empty()) {
        report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
        const IptvPreparedStreamClass guessed_class = guess_direct_stream_class(locator, {});
        switchbox::core::switch_mpv_append_debug_log_note(
            "[iptv-prepare] probe_failed locator=" + locator + " error=" + initial_probe.error_message);
        apply_class_defaults(guessed_class, "probe_failed_direct_passthrough");
        return plan;
    }

    report_progress(IptvOpenPlanStage::InspectingResponse, 0.36f);
    if (!initial_probe.looks_like_text) {
        const IptvPreparedStreamClass direct_class =
            guess_direct_stream_class(locator, initial_probe.data);
        switchbox::core::switch_mpv_append_debug_log_note(
            "[iptv-prepare] binary_probe locator=" + locator +
            " bytes=" + std::to_string(initial_probe.data.size()) +
            " hit_limit=" + std::string(initial_probe.hit_read_limit ? "true" : "false"));
        report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
        apply_class_defaults(direct_class, "binary_probe_direct");
        return plan;
    }

    if (!looks_like_hls_playlist(initial_probe.data)) {
        if (seems_like_html_or_proxy_text(initial_probe.data)) {
            report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
            plan.success = false;
            plan.stream_class = IptvPreparedStreamClass::Unsupported;
            plan.user_visible_reason = "Unsupported IPTV source: the locator resolved to an HTML/proxy page.";
            plan.debug_summary = "class=unsupported html_or_proxy_like";
            switchbox::core::switch_mpv_append_debug_log_note("[iptv-prepare] unsupported html_or_proxy_like");
            return plan;
        }

        const IptvPreparedStreamClass direct_class =
            guess_direct_stream_class(locator, initial_probe.data);
        report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
        apply_class_defaults(direct_class, "text_probe_direct_passthrough");
        switchbox::core::switch_mpv_append_debug_log_note("[iptv-prepare] passthrough locator: direct_url");
        return plan;
    }

    report_progress(IptvOpenPlanStage::ResolvingPlaylist, 0.50f);
    std::string effective_playlist_url =
        trim(initial_probe.resolved_url).empty() ? locator : trim(initial_probe.resolved_url);
    if (effective_playlist_url != locator) {
        switchbox::core::switch_mpv_append_debug_log_note(
            "[iptv-prepare] redirect_resolved from=" + locator +
            " to=" + effective_playlist_url +
            (trim(initial_probe.mime_type).empty() ? std::string() : " mime=" + trim(initial_probe.mime_type)));
    }

    std::string prepared_locator = effective_playlist_url;
    std::string prepared_playlist_text = initial_probe.data;
    IptvPreparedStreamClass stream_class = IptvPreparedStreamClass::MediaHlsLive;
    bool prepared_playlist_hit_limit = initial_probe.hit_read_limit;
    if (initial_probe.data.find("#EXT-X-STREAM-INF") != std::string::npos) {
        stream_class = IptvPreparedStreamClass::MasterHls;
        const std::string variant_url = resolve_hls_master_variant_url(effective_playlist_url, initial_probe.data);
        if (!variant_url.empty()) {
            prepared_locator = variant_url;
            switchbox::core::switch_mpv_append_debug_log_note(
                "[iptv-prepare] master_resolved variant=" + variant_url);

            report_progress(IptvOpenPlanStage::ProbingVariant, 0.64f);
            const auto variant_probe = probe_prefix_via_ffmpeg(variant_url, http_options, cancel_flag);
            if (variant_probe.error_message == "Cancelled") {
                plan.success = false;
                plan.user_visible_reason = "Cancelled";
                plan.debug_summary = "cancelled";
                return plan;
            }

            if (!variant_probe.error_message.empty()) {
                switchbox::core::switch_mpv_append_debug_log_note(
                    "[iptv-prepare] variant_prefetch_failed locator=" + variant_url +
                    " error=" + variant_probe.error_message);
            } else if (!variant_probe.looks_like_text) {
                const IptvPreparedStreamClass direct_class =
                    guess_direct_stream_class(variant_url, variant_probe.data);
                plan.final_locator = prepared_locator;
                report_progress(IptvOpenPlanStage::FinalizingPlan, 0.78f);
                apply_class_defaults(direct_class, "variant_binary_direct");
                switchbox::core::switch_mpv_append_debug_log_note(
                    "[iptv-prepare] variant_binary_passthrough locator=" + prepared_locator);
                return plan;
            } else if (looks_like_hls_playlist(variant_probe.data)) {
                prepared_playlist_text = variant_probe.data;
                prepared_playlist_hit_limit = variant_probe.hit_read_limit;
                if (is_static_hls_playlist(variant_probe.data)) {
                    prepared_locator = variant_url;
                    stream_class = IptvPreparedStreamClass::MediaHlsVod;
                    switchbox::core::switch_mpv_append_debug_log_note(
                        "[iptv-prepare] variant_vod_passthrough locator=" + variant_url +
                        " from_master=" + locator);
                }
            } else if (seems_like_html_or_proxy_text(variant_probe.data)) {
                report_progress(IptvOpenPlanStage::FinalizingPlan, 0.78f);
                plan.success = false;
                plan.stream_class = IptvPreparedStreamClass::Unsupported;
                plan.user_visible_reason = "Unsupported IPTV source: the selected HLS variant resolved to an HTML/proxy page.";
                plan.debug_summary = "class=unsupported variant_html_or_proxy_like";
                switchbox::core::switch_mpv_append_debug_log_note("[iptv-prepare] unsupported variant_html_or_proxy_like");
                return plan;
            } else {
                const IptvPreparedStreamClass direct_class =
                    guess_direct_stream_class(variant_url, variant_probe.data);
                plan.final_locator = prepared_locator;
                report_progress(IptvOpenPlanStage::FinalizingPlan, 0.78f);
                apply_class_defaults(direct_class, "variant_direct_passthrough");
                switchbox::core::switch_mpv_append_debug_log_note(
                    "[iptv-prepare] variant_direct_passthrough locator=" + prepared_locator);
                return plan;
            }
        }
    }

    if (is_static_hls_playlist(prepared_playlist_text)) {
        stream_class = IptvPreparedStreamClass::MediaHlsVod;
    } else if (stream_class != IptvPreparedStreamClass::MasterHls) {
        stream_class = IptvPreparedStreamClass::MediaHlsLive;
    }

    std::string reason;
    plan.final_locator = prepared_locator;
    if (prepared_locator != locator && prepared_locator != effective_playlist_url) {
        reason = "variant_selected";
    } else if (effective_playlist_url != locator) {
        reason = "redirect_resolved";
    } else if (prepared_playlist_hit_limit) {
        reason = "probe_limited_hls_passthrough";
    } else {
        reason = "direct_hls_passthrough";
    }
    apply_class_defaults(stream_class, std::move(reason));

    if (prepared_locator != locator &&
        stream_class == IptvPreparedStreamClass::MasterHls) {
        switchbox::core::switch_mpv_append_debug_log_note(
            "[iptv-prepare] variant_passthrough locator=" + prepared_locator);
    }

    report_progress(IptvOpenPlanStage::FinalizingPlan, 0.78f);
    switchbox::core::switch_mpv_append_debug_log_note(
        "[iptv-prepare] passthrough locator: direct_url");
#else
    (void)target;
    (void)cancel_flag;
    (void)progress_callback;
    report_progress(IptvOpenPlanStage::FinalizingPlan, 0.72f);
    apply_class_defaults(IptvPreparedStreamClass::DirectHttp, "ffmpeg_backend_unavailable");
#endif
    return plan;
}

std::string prepare_iptv_stream_locator_for_playback(
    const PlaybackTarget& target,
    const std::shared_ptr<std::atomic_bool>& cancel_flag) {
    return prepare_iptv_open_plan_for_playback(target, cancel_flag).final_locator;
}

std::string resolve_iptv_stream_url_for_playback(
    const std::string& stream_url,
    const std::shared_ptr<std::atomic_bool>& cancel_flag) {
    (void)cancel_flag;
    // Preserve the original IPTV locator and let mpv/ffmpeg walk the HLS
    // master playlist itself. Some VOD sources stall on Switch if we fetch the
    // master playlist in a separate session and then hand a rewritten child
    // variant URL to the player.
    return trim(stream_url);
}

}  // namespace switchbox::core
