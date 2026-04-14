#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace switchbox::core {

struct AppPaths {
    std::filesystem::path base_directory;
    std::filesystem::path config_file;
    std::filesystem::path languages_directory;
    std::vector<std::filesystem::path> config_search_candidates;
};

struct GeneralSettings {
    std::string language = "auto";
};

struct IptvSourceSettings {
    std::string key;
    std::string title;
    std::string url;
    bool enabled = true;
};

struct SmbSourceSettings {
    std::string key;
    std::string title;
    std::string host;
    std::string share;
    std::string username;
    std::string password;
    std::string base_path;
    bool enabled = true;
};

struct AppConfig {
    GeneralSettings general;
    std::vector<IptvSourceSettings> iptv_sources;
    std::vector<SmbSourceSettings> smb_sources;
};

class AppConfigStore {
public:
    static void set_runtime_executable_path(std::filesystem::path executable_path);
    static bool initialize();
    static bool save();

    static const AppConfig& current();
    static AppConfig& mutable_config();
    static const AppPaths& paths();
    static bool loaded_from_disk();
};

}  // namespace switchbox::core
