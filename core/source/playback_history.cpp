#include "switchbox/core/playback_history.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "switchbox/core/playback_target.hpp"

namespace switchbox::core {

namespace {

using json = nlohmann::json;

constexpr std::uint64_t kPlaybackHistoryVersion = 1;
constexpr size_t kMaxPlaybackHistoryEntries = 50;

std::string trim_copy(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::vector<std::string> split_relative_segments(std::string_view raw_path) {
    std::vector<std::string> segments;
    std::string current;

    const auto flush = [&segments, &current]() {
        std::string segment = trim_copy(current);
        current.clear();

        while (!segment.empty() && (segment.front() == '/' || segment.front() == '\\')) {
            segment.erase(segment.begin());
        }
        while (!segment.empty() && (segment.back() == '/' || segment.back() == '\\')) {
            segment.pop_back();
        }

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

std::string normalize_relative_path(std::string_view raw_path) {
    const auto segments = split_relative_segments(raw_path);
    std::string normalized;
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            normalized.push_back('/');
        }
        normalized += segments[index];
    }
    return normalized;
}

std::string source_kind_token(PlaybackSourceKind kind) {
    switch (kind) {
        case PlaybackSourceKind::Smb:
            return "smb";
        case PlaybackSourceKind::Iptv:
            return "iptv";
        default:
            return "unknown";
    }
}

PlaybackSourceKind parse_source_kind_token(const std::string& token) {
    if (token == "smb") {
        return PlaybackSourceKind::Smb;
    }
    if (token == "iptv") {
        return PlaybackSourceKind::Iptv;
    }
    return PlaybackSourceKind::Unknown;
}

std::string make_smb_stable_key(const std::string& source_key, std::string_view relative_path) {
    const std::string normalized_path = normalize_relative_path(relative_path);
    if (source_key.empty() || normalized_path.empty()) {
        return {};
    }

    return "smb:" + source_key + ":" + normalized_path;
}

std::string make_iptv_stable_key(
    const std::string& source_key,
    std::string_view entry_key,
    std::string_view stream_url_snapshot) {
    const std::string trimmed_entry_key = trim_copy(std::string(entry_key));
    if (!source_key.empty() && !trimmed_entry_key.empty()) {
        return "iptv:" + source_key + ":" + trimmed_entry_key;
    }

    const std::string trimmed_snapshot = trim_copy(std::string(stream_url_snapshot));
    if (!source_key.empty() && !trimmed_snapshot.empty()) {
        return "iptv:" + source_key + ":" + trimmed_snapshot;
    }

    return {};
}

const SmbSourceSettings* find_smb_source(const AppConfig& config, std::string_view source_key) {
    const auto it = std::find_if(
        config.smb_sources.begin(),
        config.smb_sources.end(),
        [source_key](const SmbSourceSettings& source) {
            return source.key == source_key;
        });
    if (it == config.smb_sources.end()) {
        return nullptr;
    }
    return &(*it);
}

const IptvSourceSettings* find_iptv_source(const AppConfig& config, std::string_view source_key) {
    const auto it = std::find_if(
        config.iptv_sources.begin(),
        config.iptv_sources.end(),
        [source_key](const IptvSourceSettings& source) {
            return source.key == source_key;
        });
    if (it == config.iptv_sources.end()) {
        return nullptr;
    }
    return &(*it);
}

json entry_to_json(const PlaybackHistoryEntry& entry) {
    json object = json::object();
    object["source_kind"] = source_kind_token(entry.source_kind);
    object["source_key"] = entry.source_key;
    object["source_title"] = entry.source_title;
    object["item_title"] = entry.item_title;
    object["item_subtitle"] = entry.item_subtitle;
    object["stable_key"] = entry.stable_key;
    object["last_played_at_epoch_seconds"] = entry.last_played_at_epoch_seconds;

    if (!entry.smb_relative_path.empty()) {
        object["smb_relative_path"] = entry.smb_relative_path;
    }

    if (!entry.iptv_entry_key.empty()) {
        object["iptv_entry_key"] = entry.iptv_entry_key;
    }
    if (!entry.iptv_group_title.empty()) {
        object["iptv_group_title"] = entry.iptv_group_title;
    }
    if (!entry.iptv_stream_url_snapshot.empty()) {
        object["iptv_stream_url_snapshot"] = entry.iptv_stream_url_snapshot;
    }
    if (!entry.iptv_http_user_agent.empty()) {
        object["iptv_http_user_agent"] = entry.iptv_http_user_agent;
    }
    if (!entry.iptv_http_referrer.empty()) {
        object["iptv_http_referrer"] = entry.iptv_http_referrer;
    }
    if (!entry.iptv_http_header_fields.empty()) {
        object["iptv_http_header_fields"] = entry.iptv_http_header_fields;
    }

    return object;
}

bool parse_entry_from_json(const json& object, PlaybackHistoryEntry& entry) {
    if (!object.is_object()) {
        return false;
    }

    entry = {};
    entry.source_kind = parse_source_kind_token(object.value("source_kind", std::string{}));
    entry.source_key = object.value("source_key", std::string{});
    entry.source_title = object.value("source_title", std::string{});
    entry.item_title = object.value("item_title", std::string{});
    entry.item_subtitle = object.value("item_subtitle", std::string{});
    entry.stable_key = object.value("stable_key", std::string{});
    entry.last_played_at_epoch_seconds = object.value("last_played_at_epoch_seconds", std::uint64_t{0});
    entry.smb_relative_path = object.value("smb_relative_path", std::string{});
    entry.iptv_entry_key = object.value("iptv_entry_key", std::string{});
    entry.iptv_group_title = object.value("iptv_group_title", std::string{});
    entry.iptv_stream_url_snapshot = object.value("iptv_stream_url_snapshot", std::string{});
    entry.iptv_http_user_agent = object.value("iptv_http_user_agent", std::string{});
    entry.iptv_http_referrer = object.value("iptv_http_referrer", std::string{});

    if (const auto headers = object.find("iptv_http_header_fields");
        headers != object.end() && headers->is_array()) {
        for (const auto& header : *headers) {
            if (header.is_string()) {
                entry.iptv_http_header_fields.push_back(header.get<std::string>());
            }
        }
    }

    if (entry.source_kind == PlaybackSourceKind::Smb) {
        entry.smb_relative_path = normalize_relative_path(entry.smb_relative_path);
        if (entry.stable_key.empty()) {
            entry.stable_key = make_smb_stable_key(entry.source_key, entry.smb_relative_path);
        }
    } else if (entry.source_kind == PlaybackSourceKind::Iptv) {
        if (entry.stable_key.empty()) {
            entry.stable_key = make_iptv_stable_key(
                entry.source_key,
                entry.iptv_entry_key,
                entry.iptv_stream_url_snapshot);
        }
    }

    if (entry.source_kind == PlaybackSourceKind::Unknown ||
        entry.source_key.empty() ||
        entry.stable_key.empty()) {
        return false;
    }

    return true;
}

std::vector<PlaybackHistoryEntry> load_entries_from_disk(const AppPaths& paths) {
    std::ifstream input(paths.playback_history_file, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    json document;
    try {
        input >> document;
    } catch (...) {
        return {};
    }

    const auto entries_it = document.find("entries");
    if (entries_it == document.end() || !entries_it->is_array()) {
        return {};
    }

    std::vector<PlaybackHistoryEntry> entries;
    entries.reserve(entries_it->size());
    for (const auto& item : *entries_it) {
        PlaybackHistoryEntry entry;
        if (parse_entry_from_json(item, entry)) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

bool write_entries_to_disk(const AppPaths& paths, const std::vector<PlaybackHistoryEntry>& entries) {
    std::error_code error;
    std::filesystem::create_directories(paths.playback_history_file.parent_path(), error);

    json document = json::object();
    document["version"] = kPlaybackHistoryVersion;
    document["entries"] = json::array();
    for (const auto& entry : entries) {
        document["entries"].push_back(entry_to_json(entry));
    }

    const auto temporary_path = paths.playback_history_file.string() + ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }

        output << document.dump(2);
        if (!output.good()) {
            return false;
        }
    }

    std::filesystem::remove(paths.playback_history_file, error);
    error.clear();
    std::filesystem::rename(temporary_path, paths.playback_history_file, error);
    if (!error) {
        return true;
    }

    std::filesystem::remove(temporary_path, error);
    return false;
}

PlaybackHistoryEntry build_history_entry_from_target(const PlaybackTarget& target) {
    PlaybackHistoryEntry entry;
    entry.source_kind = target.source_kind;
    entry.source_key = trim_copy(target.source_key);
    entry.source_title = trim_copy(target.source_label);
    entry.item_title = trim_copy(target.title);
    entry.item_subtitle = trim_copy(target.subtitle);
    entry.last_played_at_epoch_seconds = static_cast<std::uint64_t>(std::time(nullptr));

    if (target.source_kind == PlaybackSourceKind::Smb) {
        std::optional<PlaybackTarget::SmbLocator> smb_locator = target.smb_locator;
        if (!smb_locator.has_value()) {
            PlaybackTarget::SmbLocator parsed_locator;
            if (try_parse_smb_locator_from_uri(target.primary_locator, parsed_locator)) {
                smb_locator = std::move(parsed_locator);
            }
        }

        if (smb_locator.has_value()) {
            entry.smb_relative_path = normalize_relative_path(smb_locator->relative_path);
            entry.stable_key = make_smb_stable_key(entry.source_key, entry.smb_relative_path);
        }
        return entry;
    }

    if (target.source_kind == PlaybackSourceKind::Iptv) {
        entry.iptv_entry_key = trim_copy(target.iptv_overlay_entry_key);
        entry.iptv_group_title = trim_copy(target.subtitle);
        entry.iptv_stream_url_snapshot = trim_copy(target.primary_locator);
        entry.iptv_http_user_agent = trim_copy(target.http_user_agent);
        entry.iptv_http_referrer = trim_copy(target.http_referrer);
        entry.iptv_http_header_fields = target.http_header_fields;
        entry.stable_key = make_iptv_stable_key(
            entry.source_key,
            entry.iptv_entry_key,
            entry.iptv_stream_url_snapshot);
    }

    return entry;
}

void dedupe_and_prune_entries(std::vector<PlaybackHistoryEntry>& entries) {
    std::vector<PlaybackHistoryEntry> deduped;
    deduped.reserve(std::min(entries.size(), kMaxPlaybackHistoryEntries));

    for (auto& entry : entries) {
        if (entry.stable_key.empty()) {
            continue;
        }

        const bool duplicate = std::any_of(
            deduped.begin(),
            deduped.end(),
            [&entry](const PlaybackHistoryEntry& existing) {
                return existing.stable_key == entry.stable_key;
            });
        if (!duplicate) {
            deduped.push_back(std::move(entry));
        }
        if (deduped.size() >= kMaxPlaybackHistoryEntries) {
            break;
        }
    }

    entries = std::move(deduped);
}

}  // namespace

std::vector<PlaybackHistoryEntry> load_playback_history(const AppPaths& paths) {
    return load_entries_from_disk(paths);
}

bool record_playback_history_entry(const AppPaths& paths, PlaybackHistoryEntry entry) {
    if (entry.stable_key.empty()) {
        return false;
    }

    std::vector<PlaybackHistoryEntry> entries = load_entries_from_disk(paths);
    entries.erase(
        std::remove_if(
            entries.begin(),
            entries.end(),
            [&entry](const PlaybackHistoryEntry& existing) {
                return existing.stable_key == entry.stable_key;
            }),
        entries.end());
    entries.insert(entries.begin(), std::move(entry));
    dedupe_and_prune_entries(entries);
    return write_entries_to_disk(paths, entries);
}

bool record_playback_history_for_target(
    const AppPaths& paths,
    const AppConfig& config,
    const PlaybackTarget& target) {
    switch (target.source_kind) {
        case PlaybackSourceKind::Smb: {
            const auto* source = find_smb_source(config, target.source_key);
            if (source == nullptr || !source->use_history) {
                return true;
            }
            break;
        }
        case PlaybackSourceKind::Iptv: {
            const auto* source = find_iptv_source(config, target.source_key);
            if (source == nullptr || !source->use_history) {
                return true;
            }
            break;
        }
        default:
            return true;
    }

    PlaybackHistoryEntry entry = build_history_entry_from_target(target);
    if (entry.stable_key.empty()) {
        return false;
    }

    if (entry.item_title.empty()) {
        entry.item_title = entry.source_title;
    }

    return record_playback_history_entry(paths, std::move(entry));
}

bool remove_playback_history_missing_sources(const AppPaths& paths, const AppConfig& config) {
    std::vector<PlaybackHistoryEntry> entries = load_entries_from_disk(paths);
    std::vector<PlaybackHistoryEntry> filtered;
    filtered.reserve(entries.size());

    for (auto& entry : entries) {
        const bool keep = entry.source_kind == PlaybackSourceKind::Smb
                              ? find_smb_source(config, entry.source_key) != nullptr
                              : entry.source_kind == PlaybackSourceKind::Iptv
                                    ? find_iptv_source(config, entry.source_key) != nullptr
                                    : false;
        if (keep) {
            filtered.push_back(std::move(entry));
        }
    }

    dedupe_and_prune_entries(filtered);
    if (filtered.size() == entries.size()) {
        bool identical = true;
        for (size_t index = 0; index < filtered.size(); ++index) {
            if (filtered[index].stable_key != entries[index].stable_key) {
                identical = false;
                break;
            }
        }
        if (identical) {
            return true;
        }
    }

    return write_entries_to_disk(paths, filtered);
}

bool build_playback_target_from_history_entry(
    const AppConfig& config,
    const PlaybackHistoryEntry& entry,
    PlaybackTarget& target,
    std::string& error_message) {
    if (entry.source_kind == PlaybackSourceKind::Smb) {
        const auto* source = find_smb_source(config, entry.source_key);
        if (source == nullptr) {
            error_message = "The SMB source used by this history item no longer exists.";
            return false;
        }
        if (entry.smb_relative_path.empty()) {
            error_message = "This SMB history item no longer exposes a valid relative path.";
            return false;
        }

        target = make_smb_playback_target(*source, entry.smb_relative_path);
        return true;
    }

    if (entry.source_kind == PlaybackSourceKind::Iptv) {
        const auto* source = find_iptv_source(config, entry.source_key);
        if (source == nullptr) {
            error_message = "The IPTV source used by this history item no longer exists.";
            return false;
        }
        if (trim_copy(entry.iptv_stream_url_snapshot).empty()) {
            error_message = "This IPTV history item no longer exposes a playable stream locator.";
            return false;
        }

        PlaybackTarget rebuilt_target;
        rebuilt_target.source_kind = PlaybackSourceKind::Iptv;
        rebuilt_target.source_key = source->key;
        rebuilt_target.title = !entry.item_title.empty() ? entry.item_title : source->title;
        rebuilt_target.subtitle = !entry.iptv_group_title.empty()
                                      ? entry.iptv_group_title
                                      : (!entry.item_subtitle.empty() ? entry.item_subtitle : source->title);
        rebuilt_target.source_label = !source->title.empty() ? source->title : "IPTV";
        rebuilt_target.primary_locator = trim_copy(entry.iptv_stream_url_snapshot);
        rebuilt_target.display_locator = rebuilt_target.primary_locator;
        rebuilt_target.http_user_agent = entry.iptv_http_user_agent;
        rebuilt_target.http_referrer = entry.iptv_http_referrer;
        rebuilt_target.http_header_fields = entry.iptv_http_header_fields;
        rebuilt_target.iptv_overlay_entry_key = entry.iptv_entry_key;
        rebuilt_target.locator_is_direct = !rebuilt_target.primary_locator.empty();
        target = std::move(rebuilt_target);
        return true;
    }

    error_message = "This history item uses an unsupported source type.";
    return false;
}

}  // namespace switchbox::core
