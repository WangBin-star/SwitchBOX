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
    std::string playable_extensions =
        "8svx,aac,ac3,aif,asf,avi,dv,flv,m2ts,m2v,m4a,mkv,mov,mp3,mp4,mpeg,mpg,mts,ogg,rmvb,swf,ts,vob,wav,wma,wmv,flac,m3u,m3u8,webm,jpg,gif,png,iso";
    bool show_hidden = false;
    std::string sort_order = "name_asc";
    bool hardware_decode = true;
    int short_seek = 10;
    int long_seek = 60;
    float y_hold_speed_multiplier = 2.0f;
    bool use_preferred_audio_language = false;
    std::string preferred_audio_language = "eng";
    bool use_preferred_subtitle_language = false;
    std::string preferred_subtitle_language = "eng";
    int demux_cache_sec = 20;
    int resume_start_percent = 5;
    int resume_stop_percent = 5;
    bool touch_enable = false;
    bool touch_swipe_seek = false;
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
