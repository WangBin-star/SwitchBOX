#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "switchbox/core/app_config.hpp"
#include "switchbox/core/playback_target.hpp"

namespace switchbox::core {

struct PlaybackHistoryEntry {
    PlaybackSourceKind source_kind = PlaybackSourceKind::Unknown;
    std::string source_key;
    std::string source_title;
    std::string item_title;
    std::string item_subtitle;
    std::string stable_key;
    std::uint64_t last_played_at_epoch_seconds = 0;
    double resume_position_seconds = 0.0;
    double resume_duration_seconds = 0.0;

    std::string smb_relative_path;

    std::string iptv_entry_key;
    std::string iptv_group_title;
    std::string iptv_stream_url_snapshot;
    std::string iptv_http_user_agent;
    std::string iptv_http_referrer;
    std::vector<std::string> iptv_http_header_fields;
};

std::string playback_history_stable_key_for_target(const PlaybackTarget& target);
bool playback_target_uses_history(const AppConfig& config, const PlaybackTarget& target);
std::vector<PlaybackHistoryEntry> load_playback_history(const AppPaths& paths);
bool find_playback_history_entry(
    const AppPaths& paths,
    std::string_view stable_key,
    PlaybackHistoryEntry& entry);
bool record_playback_history_entry(const AppPaths& paths, PlaybackHistoryEntry entry);
bool record_playback_history_for_target(
    const AppPaths& paths,
    const AppConfig& config,
    const PlaybackTarget& target);
bool load_playback_resume_for_target(
    const AppPaths& paths,
    const AppConfig& config,
    const PlaybackTarget& target,
    double& resume_position_seconds,
    double& resume_duration_seconds);
bool update_playback_resume_for_target(
    const AppPaths& paths,
    const AppConfig& config,
    const PlaybackTarget& target,
    double resume_position_seconds,
    double resume_duration_seconds);
bool clear_playback_resume_for_target(
    const AppPaths& paths,
    const AppConfig& config,
    const PlaybackTarget& target);
bool remove_playback_history_entry(const AppPaths& paths, std::string_view stable_key);
bool clear_playback_history(const AppPaths& paths);
bool remove_playback_history_missing_sources(const AppPaths& paths, const AppConfig& config);
bool build_playback_target_from_history_entry(
    const AppConfig& config,
    const PlaybackHistoryEntry& entry,
    PlaybackTarget& target,
    std::string& error_message);

}  // namespace switchbox::core
