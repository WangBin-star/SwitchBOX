#pragma once

#include <string>
#include <vector>

#include "switchbox/core/app_config.hpp"

namespace switchbox::core {

struct IptvPlaylistEntry {
    std::string title;
    std::string stream_url;
    std::string group_title;
    std::string logo_url;
    std::string tvg_id;
    std::string tvg_name;
    std::string tvg_country;
    std::string tvg_chno;
    std::string favorite_key;
};

struct IptvPlaylistResult {
    bool backend_available = false;
    bool success = false;
    std::string error_message;
    std::vector<IptvPlaylistEntry> entries;
};

IptvPlaylistResult load_iptv_playlist(const IptvSourceSettings& source);

}  // namespace switchbox::core
