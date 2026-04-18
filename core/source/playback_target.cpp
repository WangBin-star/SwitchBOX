#include "switchbox/core/playback_target.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

#include "switchbox/core/smb_browser.hpp"

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

std::string trim_network_component(std::string value) {
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
        std::string segment = trim_network_component(current);
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

struct ParsedShareTarget {
    std::string share_name;
    std::string initial_relative_path;
};

ParsedShareTarget parse_share_target(const std::string& raw_share) {
    const auto segments = split_relative_segments(raw_share);
    if (segments.empty()) {
        return {};
    }

    ParsedShareTarget parsed;
    parsed.share_name = segments.front();

    if (segments.size() > 1) {
        for (size_t index = 1; index < segments.size(); ++index) {
            if (!parsed.initial_relative_path.empty()) {
                parsed.initial_relative_path += '/';
            }

            parsed.initial_relative_path += segments[index];
        }
    }

    return parsed;
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

        decoded.push_back(character);
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

std::string build_userinfo(
    const std::string& username,
    const std::string& password,
    bool redact_password) {
    if (username.empty()) {
        return {};
    }

    std::string userinfo = percent_encode(username);
    if (!password.empty()) {
        userinfo.push_back(':');
        userinfo += redact_password ? "******" : percent_encode(password);
    }

    userinfo.push_back('@');
    return userinfo;
}

std::string build_smb_uri(
    const SmbSourceSettings& source,
    const std::string& relative_path,
    bool redact_password) {
    const std::string host = trim_network_component(source.host);
    const ParsedShareTarget share_target = parse_share_target(source.share);

    if (host.empty() || share_target.share_name.empty()) {
        return {};
    }

    std::vector<std::string> full_segments;
    full_segments.push_back(share_target.share_name);

    const auto initial_segments = split_relative_segments(share_target.initial_relative_path);
    full_segments.insert(full_segments.end(), initial_segments.begin(), initial_segments.end());

    const auto relative_segments = split_relative_segments(relative_path);
    full_segments.insert(full_segments.end(), relative_segments.begin(), relative_segments.end());

    return "smb://" +
           build_userinfo(source.username, source.password, redact_password) +
           percent_encode(host) +
           "/" +
           encode_path_segments(full_segments);
}

std::string build_windows_unc_path(
    const SmbSourceSettings& source,
    const std::string& relative_path) {
    const std::string host = trim_network_component(source.host);
    const ParsedShareTarget share_target = parse_share_target(source.share);

    if (host.empty() || share_target.share_name.empty()) {
        return {};
    }

    std::vector<std::string> full_segments;
    full_segments.push_back(share_target.share_name);

    const auto initial_segments = split_relative_segments(share_target.initial_relative_path);
    full_segments.insert(full_segments.end(), initial_segments.begin(), initial_segments.end());

    const auto relative_segments = split_relative_segments(relative_path);
    full_segments.insert(full_segments.end(), relative_segments.begin(), relative_segments.end());

    std::string path = "\\\\" + host;
    for (const auto& segment : full_segments) {
        path += "\\";
        path += segment;
    }

    return path;
}

}  // namespace

PlaybackTarget make_smb_playback_target(
    const SmbSourceSettings& source,
    const std::string& relative_path) {
    PlaybackTarget target;
    target.source_kind = PlaybackSourceKind::Smb;
    target.title = relative_path.empty() ? source.title : split_relative_segments(relative_path).back();
    target.subtitle = smb_display_path(source, relative_path);
    target.source_label = smb_source_root_label(source);
    target.primary_locator = build_smb_uri(source, relative_path, false);
    target.display_locator = build_smb_uri(source, relative_path, true);
    target.fallback_locator = build_windows_unc_path(source, relative_path);
    target.locator_is_direct = !target.primary_locator.empty();
    target.smb_locator = PlaybackTarget::SmbLocator{
        .host = source.host,
        .share = source.share,
        .username = source.username,
        .password = source.password,
        .relative_path = relative_path,
    };
    return target;
}

PlaybackTarget make_iptv_playback_target(
    const IptvSourceSettings& source,
    const IptvPlaylistEntry& entry,
    std::shared_ptr<const IptvPlaybackOverlayContext> overlay_context,
    size_t overlay_group_index) {
    PlaybackTarget target;
    target.source_kind = PlaybackSourceKind::Iptv;
    target.title = !entry.title.empty() ? entry.title : trim(source.title);
    target.subtitle = !entry.group_title.empty() ? entry.group_title : trim(source.title);
    target.source_label = trim(source.title).empty() ? "IPTV" : trim(source.title);
    target.primary_locator = trim(entry.stream_url);
    target.display_locator = target.primary_locator;
    target.fallback_locator.clear();
    target.http_user_agent = trim(entry.http_user_agent);
    target.http_referrer = trim(entry.http_referrer);
    target.http_header_fields = entry.http_header_fields;
    target.iptv_overlay_context = std::move(overlay_context);
    target.iptv_overlay_group_index = overlay_group_index;
    target.iptv_overlay_entry_key = entry.favorite_key;
    target.locator_pre_resolved = false;
    target.locator_is_direct = !target.primary_locator.empty();
    return target;
}

bool try_parse_smb_locator_from_uri(
    std::string_view locator_uri,
    PlaybackTarget::SmbLocator& smb_locator) {
    constexpr std::string_view prefix = "smb://";
    if (!locator_uri.starts_with(prefix)) {
        return false;
    }

    std::string_view remainder = locator_uri.substr(prefix.size());
    const size_t first_slash = remainder.find('/');
    if (first_slash == std::string_view::npos) {
        return false;
    }

    const std::string_view authority = remainder.substr(0, first_slash);
    const std::string_view encoded_path = remainder.substr(first_slash + 1);
    if (authority.empty() || encoded_path.empty()) {
        return false;
    }

    std::string_view userinfo;
    std::string_view host = authority;
    const size_t at_sign = authority.rfind('@');
    if (at_sign != std::string_view::npos) {
        userinfo = authority.substr(0, at_sign);
        host = authority.substr(at_sign + 1);
    }

    std::string username;
    std::string password;
    if (!userinfo.empty()) {
        const size_t colon = userinfo.find(':');
        if (colon == std::string_view::npos) {
            username = percent_decode_component(userinfo);
        } else {
            username = percent_decode_component(userinfo.substr(0, colon));
            password = percent_decode_component(userinfo.substr(colon + 1));
        }
    }

    std::vector<std::string> path_segments;
    size_t segment_begin = 0;
    while (segment_begin <= encoded_path.size()) {
        const size_t separator = encoded_path.find('/', segment_begin);
        const std::string_view encoded_segment =
            separator == std::string_view::npos
                ? encoded_path.substr(segment_begin)
                : encoded_path.substr(segment_begin, separator - segment_begin);
        if (!encoded_segment.empty()) {
            path_segments.push_back(percent_decode_component(encoded_segment));
        }

        if (separator == std::string_view::npos) {
            break;
        }
        segment_begin = separator + 1;
    }

    if (path_segments.size() < 2) {
        return false;
    }

    std::string share;
    for (size_t index = 0; index + 1 < path_segments.size(); ++index) {
        if (!share.empty()) {
            share += '/';
        }
        share += path_segments[index];
    }

    smb_locator.host = percent_decode_component(host);
    smb_locator.share = std::move(share);
    smb_locator.username = std::move(username);
    smb_locator.password = std::move(password);
    smb_locator.relative_path = path_segments.back();
    return !smb_locator.host.empty() && !smb_locator.share.empty() && !smb_locator.relative_path.empty();
}

}  // namespace switchbox::core
