#pragma once

#include <optional>
#include <string>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/iptv_playlist.hpp"

namespace switchbox::core {

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
    bool locator_is_direct = false;
    std::optional<SmbLocator> smb_locator;
};

PlaybackTarget make_smb_playback_target(
    const SmbSourceSettings& source,
    const std::string& relative_path);
PlaybackTarget make_iptv_playback_target(
    const IptvSourceSettings& source,
    const IptvPlaylistEntry& entry);

}  // namespace switchbox::core
