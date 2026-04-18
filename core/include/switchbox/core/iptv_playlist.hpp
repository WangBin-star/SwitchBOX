#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "switchbox/core/app_config.hpp"

namespace switchbox::core {

struct PlaybackTarget;

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
    std::string http_user_agent;
    std::string http_referrer;
    std::vector<std::string> http_header_fields;
};

struct IptvPlaylistResult {
    bool backend_available = false;
    bool success = false;
    std::string error_message;
    std::vector<IptvPlaylistEntry> entries;
};

enum class IptvPlaylistLoadStage {
    Starting,
    OpeningConnection,
    DownloadingPlaylist,
    ParsingPlaylist,
    Finalizing,
};

struct IptvPlaylistLoadProgress {
    float progress = 0.0f;
    IptvPlaylistLoadStage stage = IptvPlaylistLoadStage::Starting;
    std::uint64_t bytes_read = 0;
    std::uint64_t total_bytes = 0;
};

using IptvPlaylistProgressCallback = std::function<void(const IptvPlaylistLoadProgress&)>;

enum class IptvOpenPlanStage {
    Starting,
    NormalizingLocator,
    ProbingSource,
    InspectingResponse,
    ResolvingPlaylist,
    ProbingVariant,
    FinalizingPlan,
};

struct IptvOpenPlanProgress {
    float progress = 0.0f;
    IptvOpenPlanStage stage = IptvOpenPlanStage::Starting;
};

using IptvOpenPlanProgressCallback = std::function<void(const IptvOpenPlanProgress&)>;

enum class IptvPreparedStreamClass {
    Unknown,
    DirectHttp,
    DirectFlv,
    MasterHls,
    MediaHlsLive,
    MediaHlsVod,
    DirectTs,
    Unsupported,
};

struct IptvOpenPlan {
    bool success = true;
    IptvPreparedStreamClass stream_class = IptvPreparedStreamClass::Unknown;
    std::string final_locator;
    std::vector<std::pair<std::string, std::string>> option_pairs;
    std::string analyzeduration_override;
    std::string probescore_override;
    std::string effective_user_agent;
    std::string effective_referrer;
    std::vector<std::string> effective_header_fields;
    std::string debug_summary;
    std::string user_visible_reason;
};

IptvPlaylistResult load_iptv_playlist(
    const IptvSourceSettings& source,
    const std::shared_ptr<std::atomic_bool>& cancel_flag = {},
    IptvPlaylistProgressCallback progress_callback = {});

IptvOpenPlan prepare_iptv_open_plan_for_playback(
    const PlaybackTarget& target,
    const std::shared_ptr<std::atomic_bool>& cancel_flag = {},
    IptvOpenPlanProgressCallback progress_callback = {});

std::string prepare_iptv_stream_locator_for_playback(
    const PlaybackTarget& target,
    const std::shared_ptr<std::atomic_bool>& cancel_flag = {});

std::string resolve_iptv_stream_url_for_playback(
    const std::string& stream_url,
    const std::shared_ptr<std::atomic_bool>& cancel_flag = {});

}  // namespace switchbox::core
