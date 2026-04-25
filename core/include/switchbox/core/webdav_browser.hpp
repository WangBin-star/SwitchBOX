#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "switchbox/core/app_config.hpp"

namespace switchbox::core {

struct WebDavBrowserEntry {
    std::string name;
    std::string relative_path;
    bool is_directory = false;
    bool playable = false;
    std::uintmax_t size = 0;
    std::int64_t modified_timestamp = 0;
};

struct WebDavBrowserResult {
    bool success = false;
    bool backend_available = false;
    std::string root_label;
    std::string requested_path;
    std::string resolved_path;
    std::string error_message;
    std::vector<std::string> normalized_extensions;
    std::vector<WebDavBrowserEntry> entries;
};

enum class WebDavBrowseLoadStage {
    Starting,
    OpeningConnection,
    ParsingResponse,
    Finalizing,
};

struct WebDavBrowseLoadProgress {
    WebDavBrowseLoadStage stage = WebDavBrowseLoadStage::Starting;
    float progress = 0.0f;
};

struct WebDavFileProbeResult {
    bool success = false;
    bool backend_available = false;
    std::uintmax_t file_size = 0;
    std::string error_message;
};

struct WebDavFileReadResult {
    bool success = false;
    bool backend_available = false;
    std::uintmax_t file_size = 0;
    std::uintmax_t response_offset = 0;
    bool eof = false;
    std::string error_message;
    std::vector<char> bytes;
};

std::string webdav_source_root_label(const WebDavSourceSettings& source);
std::string webdav_join_relative_path(const std::string& base_path, const std::string& child_name);
std::string webdav_parent_relative_path(const std::string& relative_path);
std::string webdav_display_path(const WebDavSourceSettings& source, const std::string& relative_path);
std::string webdav_file_url(const WebDavSourceSettings& source, const std::string& relative_path);
std::string webdav_authorized_file_url(const WebDavSourceSettings& source, const std::string& relative_path);
std::string webdav_directory_url(const WebDavSourceSettings& source, const std::string& relative_path);
std::string webdav_build_basic_auth_header(const WebDavSourceSettings& source);

WebDavBrowserResult browse_webdav_directory(
    const WebDavSourceSettings& source,
    const GeneralSettings& general,
    const std::string& relative_path,
    std::function<void(const WebDavBrowseLoadProgress&)> progress_callback = {});

WebDavFileProbeResult probe_webdav_file(
    const WebDavSourceSettings& source,
    const std::string& relative_path);

WebDavFileReadResult read_webdav_file_range(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::uintmax_t offset,
    std::size_t max_bytes);

bool delete_webdav_file(
    const WebDavSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message);

}  // namespace switchbox::core
