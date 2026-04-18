#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/iptv_playlist.hpp"

namespace switchbox::core {

struct IptvPlaybackOverlayGroup {
    std::string title;
    std::vector<size_t> entry_indices;
    bool favorites = false;
};

struct IptvPlaybackOverlayContext {
    IptvSourceSettings source;
    std::vector<IptvPlaylistEntry> entries;
    std::vector<IptvPlaybackOverlayGroup> groups;
};

enum class PlaybackSourceKind {
    Unknown,
    Smb,
    Iptv,
};

struct PlaybackTarget {
    struct SmbLocator {
        std::string host;
        std::string share;
        std::string username;
        std::string password;
        std::string relative_path;
    };

    PlaybackSourceKind source_kind = PlaybackSourceKind::Unknown;
    std::string title;
    std::string subtitle;
    std::string source_label;
    std::string display_locator;
    std::string primary_locator;
    std::string fallback_locator;
    std::string http_user_agent;
    std::string http_referrer;
    std::vector<std::string> http_header_fields;
    std::optional<IptvOpenPlan> iptv_open_plan;
    std::shared_ptr<const IptvPlaybackOverlayContext> iptv_overlay_context;
    size_t iptv_overlay_group_index = 0;
    std::string iptv_overlay_entry_key;
    bool locator_pre_resolved = false;
    bool locator_is_direct = false;
    std::optional<SmbLocator> smb_locator;
};

PlaybackTarget make_smb_playback_target(
    const SmbSourceSettings& source,
    const std::string& relative_path);
PlaybackTarget make_iptv_playback_target(
    const IptvSourceSettings& source,
    const IptvPlaylistEntry& entry,
    std::shared_ptr<const IptvPlaybackOverlayContext> overlay_context = nullptr,
    size_t overlay_group_index = 0);
bool try_parse_smb_locator_from_uri(
    std::string_view locator_uri,
    PlaybackTarget::SmbLocator& smb_locator);

}  // namespace switchbox::core
