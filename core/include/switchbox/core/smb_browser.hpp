#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "switchbox/core/app_config.hpp"

namespace switchbox::core {

struct SmbBrowserEntry {
    std::string name;
    std::string relative_path;
    bool is_directory = false;
    bool playable = false;
    std::uintmax_t size = 0;
};

struct SmbBrowserResult {
    bool success = false;
    bool backend_available = false;
    std::string root_label;
    std::string requested_path;
    std::string resolved_path;
    std::string error_message;
    std::vector<std::string> normalized_extensions;
    std::vector<SmbBrowserEntry> entries;
};

std::vector<std::string> normalize_playable_extensions(const std::string& raw_extensions);
bool is_playable_extension(
    const std::string& file_name,
    const std::vector<std::string>& normalized_extensions);

std::string smb_source_root_label(const SmbSourceSettings& source);
std::string smb_join_relative_path(const std::string& base_path, const std::string& child_name);
std::string smb_parent_relative_path(const std::string& relative_path);
std::string smb_display_path(const SmbSourceSettings& source, const std::string& relative_path);

SmbBrowserResult browse_smb_directory(
    const SmbSourceSettings& source,
    const GeneralSettings& general,
    const std::string& relative_path);

bool delete_smb_file(
    const SmbSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message);

}  // namespace switchbox::core
