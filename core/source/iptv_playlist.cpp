#include "switchbox/core/iptv_playlist.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "switchbox/core/build_info.hpp"

#if __has_include(<libavformat/avio.h>) && __has_include(<libavformat/avformat.h>) && __has_include(<libavutil/error.h>)
#define SWITCHBOX_HAS_IPTV_FFMPEG_IO 1
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
}
#else
#define SWITCHBOX_HAS_IPTV_FFMPEG_IO 0
#endif

namespace switchbox::core {

namespace {

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

void finalize_playlist_entry(IptvPlaylistEntry& entry) {
    entry.title = sanitize_text(std::move(entry.title));
    entry.group_title = sanitize_text(std::move(entry.group_title));
    entry.tvg_name = sanitize_text(std::move(entry.tvg_name));
    entry.tvg_id = sanitize_text(std::move(entry.tvg_id));
    entry.tvg_country = sanitize_text(std::move(entry.tvg_country));
    entry.tvg_chno = sanitize_text(std::move(entry.tvg_chno));
    entry.favorite_key = make_iptv_entry_favorite_key(entry);
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
std::string av_error_to_string(int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer {};
    av_strerror(error_code, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

std::string fetch_text_via_ffmpeg(const std::string& url, std::string& error_message) {
    error_message.clear();
    avformat_network_init();

    AVDictionary* options = nullptr;
    AVIOContext* io_context = nullptr;
    const std::string timeout_us = "30000000";
    const std::string user_agent =
        switchbox::core::BuildInfo::app_name() + "/" + switchbox::core::BuildInfo::version_string();

    av_dict_set(&options, "user_agent", user_agent.c_str(), 0);
    av_dict_set(&options, "timeout", timeout_us.c_str(), 0);
    av_dict_set(&options, "rw_timeout", timeout_us.c_str(), 0);

    const int open_result = avio_open2(&io_context, url.c_str(), AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);
    if (open_result < 0) {
        error_message = "Open playlist failed: " + av_error_to_string(open_result);
        return {};
    }

    std::string text;
    const int64_t size = avio_size(io_context);
    if (size > 0 && size <= 8 * 1024 * 1024) {
        text.reserve(static_cast<size_t>(size));
    }

    std::array<unsigned char, 32 * 1024> buffer {};
    while (true) {
        const int read_result = avio_read(io_context, buffer.data(), static_cast<int>(buffer.size()));
        if (read_result == AVERROR_EOF || read_result == 0) {
            break;
        }

        if (read_result < 0) {
            error_message = "Read playlist failed: " + av_error_to_string(read_result);
            avio_closep(&io_context);
            return {};
        }

        text.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(read_result));
    }

    avio_closep(&io_context);
    return text;
}
#endif

}  // namespace

IptvPlaylistResult load_iptv_playlist(const IptvSourceSettings& source) {
    IptvPlaylistResult result;
#if SWITCHBOX_HAS_IPTV_FFMPEG_IO
    result.backend_available = true;

    if (trim(source.url).empty()) {
        result.error_message = "Playlist URL is not configured.";
        return result;
    }

    std::string error_message;
    const std::string playlist_text = fetch_text_via_ffmpeg(source.url, error_message);
    if (!error_message.empty()) {
        result.error_message = std::move(error_message);
        return result;
    }

    if (playlist_text.empty()) {
        result.error_message = "Playlist is empty.";
        return result;
    }

    if (looks_like_hls_playlist(playlist_text)) {
        IptvPlaylistEntry entry;
        entry.title = source.title.empty() ? fallback_title_from_url(source.url) : source.title;
        entry.stream_url = source.url;
        finalize_playlist_entry(entry);
        result.entries.push_back(std::move(entry));
        result.success = true;
        return result;
    }

    result.entries = parse_iptv_playlist_text(source.url, playlist_text);
    if (result.entries.empty()) {
        result.error_message = "Playlist does not contain any playable channels.";
        return result;
    }

    result.success = true;
    return result;
#else
    (void)source;
    result.backend_available = false;
    result.error_message = "IPTV playlist backend is not available in this build.";
    return result;
#endif
}

}  // namespace switchbox::core
