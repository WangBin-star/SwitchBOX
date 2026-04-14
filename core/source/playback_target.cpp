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

}  // namespace switchbox::core
