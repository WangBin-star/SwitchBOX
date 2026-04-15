#include "switchbox/core/smb_browser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>

#if defined(_WIN32) && !defined(__SWITCH__)
#include <windows.h>
#include <winnetwk.h>
#endif

#ifdef __SWITCH__
#include <fcntl.h>
#include <smb2/libsmb2.h>
#include <smb2/smb2.h>
#endif

namespace switchbox::core {

namespace {

struct ParsedShareTarget {
    std::string share_name;
    std::string initial_relative_path;
};

#ifdef __SWITCH__
struct Smb2ContextDeleter {
    void operator()(smb2_context* context) const {
        if (context != nullptr) {
            smb2_destroy_context(context);
        }
    }
};

struct Smb2DirectoryDeleter {
    smb2_context* context = nullptr;

    void operator()(smb2dir* directory) const {
        if (context != nullptr && directory != nullptr) {
            smb2_closedir(context, directory);
        }
    }
};
#endif

#if defined(_WIN32) && !defined(__SWITCH__)
struct WindowsSmbAuthResult {
    DWORD error_code = NO_ERROR;
    std::string message;

    [[nodiscard]] bool success() const {
        return error_code == NO_ERROR;
    }
};
#endif

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim_network_component(std::string value) {
    value = trim(std::move(value));

    while (!value.empty() && (value.front() == '/' || value.front() == '\\')) {
        value.erase(value.begin());
    }

    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }

    return value;
}

std::vector<std::string> split_relative_segments(std::string_view raw_path) {
    std::vector<std::string> segments;
    std::string current;

    const auto flush = [&segments, &current]() {
        std::string segment = trim_network_component(current);
        current.clear();

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

std::string join_segments(const std::vector<std::string>& segments) {
    std::string path;

    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            path += '/';
        }

        path += segments[index];
    }

    return path;
}

std::filesystem::path path_from_utf8(const std::string& value);

std::filesystem::path append_relative_segments(
    std::filesystem::path base_path,
    const std::vector<std::string>& segments) {
    for (const auto& segment : segments) {
        base_path /= path_from_utf8(segment);
    }

    return base_path;
}

std::string path_string(const std::filesystem::path& path) {
    const auto native = path.generic_u8string();
    return std::string(native.begin(), native.end());
}

std::filesystem::path path_from_utf8(const std::string& value) {
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

#if defined(_WIN32) && !defined(__SWITCH__)
std::wstring utf8_to_wstring(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int wide_size = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (wide_size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(wide_size), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        wide.data(),
        wide_size);
    if (converted != wide_size) {
        return {};
    }

    return wide;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_size <= 0) {
        return {};
    }

    std::string utf8(static_cast<size_t>(utf8_size), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        utf8.data(),
        utf8_size,
        nullptr,
        nullptr);
    if (converted != utf8_size) {
        return {};
    }

    return utf8;
}

std::string format_windows_error_message(DWORD error_code) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message;
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, buffer + length);
        LocalFree(buffer);
    }

    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }

    if (message.empty()) {
        return "Windows error " + std::to_string(error_code);
    }

    return wide_to_utf8(message);
}

bool remote_name_matches_host(const std::wstring& remote_name, const std::wstring& host_name) {
    if (remote_name.empty() || host_name.empty()) {
        return false;
    }

    const std::wstring prefix = L"\\\\";
    if (!remote_name.starts_with(prefix)) {
        return false;
    }

    size_t host_start = prefix.size();
    size_t host_end = remote_name.find(L'\\', host_start);
    std::wstring remote_host =
        host_end == std::wstring::npos ? remote_name.substr(host_start)
                                       : remote_name.substr(host_start, host_end - host_start);

    std::transform(remote_host.begin(), remote_host.end(), remote_host.begin(), ::towlower);
    std::wstring normalized_host = host_name;
    std::transform(normalized_host.begin(), normalized_host.end(), normalized_host.begin(), ::towlower);
    return remote_host == normalized_host;
}

void collect_connected_local_names_for_host(
    const std::wstring& host_name,
    std::vector<std::wstring>& local_names) {
    const DWORD logical_drives = GetLogicalDrives();
    if (logical_drives == 0) {
        return;
    }

    for (int index = 0; index < 26; ++index) {
        if ((logical_drives & (1u << index)) == 0) {
            continue;
        }

        wchar_t drive_name[] = {static_cast<wchar_t>(L'A' + index), L':', L'\0'};
        DWORD buffer_size = 2048;
        std::wstring remote_name(static_cast<size_t>(buffer_size), L'\0');
        DWORD result = WNetGetConnectionW(drive_name, remote_name.data(), &buffer_size);
        if (result == ERROR_MORE_DATA) {
            remote_name.resize(buffer_size);
            result = WNetGetConnectionW(drive_name, remote_name.data(), &buffer_size);
        }

        if (result != NO_ERROR) {
            continue;
        }

        if (buffer_size == 0) {
            continue;
        }

        remote_name.resize(buffer_size > 0 ? buffer_size - 1 : 0);
        if (remote_name_matches_host(remote_name, host_name)) {
            local_names.emplace_back(drive_name);
        }
    }
}

void collect_connected_remote_names_for_host(
    HANDLE enum_handle,
    const std::wstring& host_name,
    std::vector<std::wstring>& remote_names,
    std::vector<std::wstring>* local_names = nullptr) {
    std::array<std::byte, 32 * 1024> buffer{};

    while (true) {
        DWORD entry_count = 0xFFFFFFFF;
        DWORD buffer_size = static_cast<DWORD>(buffer.size());
        DWORD result = WNetEnumResourceW(
            enum_handle,
            &entry_count,
            buffer.data(),
            &buffer_size);

        if (result == ERROR_NO_MORE_ITEMS) {
            return;
        }

        if (result == ERROR_MORE_DATA) {
            return;
        }

        if (result != NO_ERROR) {
            return;
        }

        auto* resources = reinterpret_cast<NETRESOURCEW*>(buffer.data());
        for (DWORD index = 0; index < entry_count; ++index) {
            const NETRESOURCEW& resource = resources[index];
            if (local_names != nullptr && resource.lpLocalName != nullptr) {
                local_names->emplace_back(resource.lpLocalName);
            }
            if (resource.lpRemoteName != nullptr) {
                std::wstring remote_name = resource.lpRemoteName;
                if (remote_name_matches_host(remote_name, host_name)) {
                    remote_names.push_back(std::move(remote_name));
                }
            }
        }
    }
}

void disconnect_windows_smb_host_connections(const std::string& host) {
    const std::wstring host_name = utf8_to_wstring(host);
    if (host_name.empty()) {
        return;
    }

    std::vector<std::wstring> local_names;
    collect_connected_local_names_for_host(host_name, local_names);

    std::sort(local_names.begin(), local_names.end());
    local_names.erase(std::unique(local_names.begin(), local_names.end()), local_names.end());
    for (const auto& local_name : local_names) {
        WNetCancelConnection2W(local_name.c_str(), CONNECT_UPDATE_PROFILE, TRUE);
    }

    std::vector<std::wstring> remembered_local_names;
    std::vector<std::wstring> remembered_remote_names;

    HANDLE enum_handle = nullptr;
    const DWORD open_result = WNetOpenEnumW(
        RESOURCE_CONNECTED,
        RESOURCETYPE_DISK,
        0,
        nullptr,
        &enum_handle);
    if (open_result != NO_ERROR || enum_handle == nullptr) {
        enum_handle = nullptr;
    }

    if (enum_handle != nullptr) {
        std::vector<std::wstring> remote_names;
        collect_connected_remote_names_for_host(enum_handle, host_name, remote_names);
        WNetCloseEnum(enum_handle);

        std::sort(remote_names.begin(), remote_names.end());
        remote_names.erase(std::unique(remote_names.begin(), remote_names.end()), remote_names.end());

        for (const auto& remote_name : remote_names) {
            WNetCancelConnection2W(remote_name.c_str(), CONNECT_UPDATE_PROFILE, TRUE);
        }
    }

    const DWORD remembered_result = WNetOpenEnumW(
        RESOURCE_REMEMBERED,
        RESOURCETYPE_DISK,
        0,
        nullptr,
        &enum_handle);
    if (remembered_result != NO_ERROR || enum_handle == nullptr) {
        return;
    }

    collect_connected_remote_names_for_host(
        enum_handle,
        host_name,
        remembered_remote_names,
        &remembered_local_names);
    WNetCloseEnum(enum_handle);

    std::sort(remembered_local_names.begin(), remembered_local_names.end());
    remembered_local_names.erase(
        std::unique(remembered_local_names.begin(), remembered_local_names.end()),
        remembered_local_names.end());
    for (const auto& local_name : remembered_local_names) {
        WNetCancelConnection2W(local_name.c_str(), CONNECT_UPDATE_PROFILE, TRUE);
    }

    std::sort(remembered_remote_names.begin(), remembered_remote_names.end());
    remembered_remote_names.erase(
        std::unique(remembered_remote_names.begin(), remembered_remote_names.end()),
        remembered_remote_names.end());
    for (const auto& remote_name : remembered_remote_names) {
        WNetCancelConnection2W(remote_name.c_str(), CONNECT_UPDATE_PROFILE, TRUE);
    }
}

WindowsSmbAuthResult ensure_windows_smb_connection(
    const std::string& host,
    const ParsedShareTarget& share_target,
    const std::string& username,
    const std::string& password) {
    const std::wstring remote_name = utf8_to_wstring("\\\\" + host + "\\" + share_target.share_name);
    if (remote_name.empty()) {
        return {
            .error_code = ERROR_INVALID_NAME,
            .message = "Unable to convert SMB path for Windows authentication.",
        };
    }

    std::wstring username_wide = utf8_to_wstring(username);
    std::wstring password_wide = utf8_to_wstring(password);

    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = const_cast<LPWSTR>(remote_name.c_str());

    const DWORD result = WNetAddConnection2W(
        &resource,
        password_wide.empty() ? nullptr : password_wide.c_str(),
        username_wide.empty() ? nullptr : username_wide.c_str(),
        CONNECT_TEMPORARY);

    if (result == NO_ERROR ||
        result == ERROR_ALREADY_ASSIGNED ||
        result == ERROR_DEVICE_ALREADY_REMEMBERED) {
        return {};
    }

    if (result == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        return {
            .error_code = result,
            .message = format_windows_error_message(result),
        };
    }

    return {
        .error_code = result,
        .message = format_windows_error_message(result),
    };
}
#endif

ParsedShareTarget parse_share_target(const std::string& raw_share) {
    const auto segments = split_relative_segments(raw_share);
    if (segments.empty()) {
        return {};
    }

    ParsedShareTarget parsed;
    parsed.share_name = segments.front();

    if (segments.size() > 1) {
        parsed.initial_relative_path = join_segments(
            std::vector<std::string>(segments.begin() + 1, segments.end()));
    }

    return parsed;
}

std::string make_smb2_path(const ParsedShareTarget& share_target, const std::string& relative_path) {
    std::vector<std::string> segments = split_relative_segments(share_target.initial_relative_path);
    const auto relative_segments = split_relative_segments(relative_path);
    segments.insert(segments.end(), relative_segments.begin(), relative_segments.end());
    const std::string effective_relative_path = join_segments(segments);

    if (effective_relative_path.empty()) {
        return {};
    }

    return effective_relative_path;
}

bool entry_name_less(const SmbBrowserEntry& lhs, const SmbBrowserEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory && !rhs.is_directory;
    }

    return to_lower(lhs.name) < to_lower(rhs.name);
}

#ifdef __SWITCH__
bool delete_smb_path_recursive(
    smb2_context* smb2,
    const std::string& smb2_path,
    std::string& error_message) {
    if (smb2 == nullptr || smb2_path.empty()) {
        error_message = "SMB file path is empty.";
        return false;
    }

    smb2_stat_64 entry_stat{};
    if (smb2_stat(smb2, smb2_path.c_str(), &entry_stat) < 0) {
        error_message = smb2_get_error(smb2);
        return false;
    }

    if (entry_stat.smb2_type != SMB2_TYPE_DIRECTORY) {
        if (smb2_unlink(smb2, smb2_path.c_str()) < 0) {
            error_message = smb2_get_error(smb2);
            return false;
        }
        return true;
    }

    std::unique_ptr<smb2dir, Smb2DirectoryDeleter> directory(
        smb2_opendir(smb2, smb2_path.c_str()),
        Smb2DirectoryDeleter{smb2});
    if (!directory) {
        error_message = smb2_get_error(smb2);
        return false;
    }

    while (auto* entry = smb2_readdir(smb2, directory.get())) {
        const std::string name = entry->name == nullptr ? "" : entry->name;
        if (name.empty() || name == "." || name == "..") {
            continue;
        }

        const std::string child_path = smb2_path + "/" + name;
        if (!delete_smb_path_recursive(smb2, child_path, error_message)) {
            return false;
        }
    }

    directory.reset();
    if (smb2_rmdir(smb2, smb2_path.c_str()) < 0) {
        error_message = smb2_get_error(smb2);
        return false;
    }

    return true;
}
#endif

}  // namespace

std::vector<std::string> normalize_playable_extensions(const std::string& raw_extensions) {
    std::vector<std::string> normalized;
    std::string current;

    const auto flush = [&normalized, &current]() {
        std::string value = to_lower(trim(current));
        current.clear();

        if (value.empty()) {
            return;
        }

        if (!value.starts_with('.')) {
            value.insert(value.begin(), '.');
        }

        if (std::find(normalized.begin(), normalized.end(), value) == normalized.end()) {
            normalized.push_back(std::move(value));
        }
    };

    for (const char character : raw_extensions) {
        if (character == ',') {
            flush();
            continue;
        }

        current.push_back(character);
    }

    flush();
    return normalized;
}

bool is_playable_extension(
    const std::string& file_name,
    const std::vector<std::string>& normalized_extensions) {
    const std::string extension = to_lower(path_string(path_from_utf8(file_name).extension()));

    if (extension.empty()) {
        return false;
    }

    return std::find(
               normalized_extensions.begin(),
               normalized_extensions.end(),
               extension) != normalized_extensions.end();
}

std::string smb_source_root_label(const SmbSourceSettings& source) {
    const std::string host = trim_network_component(source.host);
    const std::string share = join_segments(split_relative_segments(source.share));

    if (!host.empty() && !share.empty()) {
        return host + "/" + share;
    }

    if (!host.empty()) {
        return host;
    }

    if (!share.empty()) {
        return share;
    }

    return {};
}

std::string smb_join_relative_path(const std::string& base_path, const std::string& child_name) {
    std::vector<std::string> segments = split_relative_segments(base_path);
    const auto child_segments = split_relative_segments(child_name);
    segments.insert(segments.end(), child_segments.begin(), child_segments.end());
    return join_segments(segments);
}

std::string smb_parent_relative_path(const std::string& relative_path) {
    std::vector<std::string> segments = split_relative_segments(relative_path);

    if (!segments.empty()) {
        segments.pop_back();
    }

    return join_segments(segments);
}

std::string smb_display_path(const SmbSourceSettings& source, const std::string& relative_path) {
    const std::string root = smb_source_root_label(source);
    const std::string normalized_relative = join_segments(split_relative_segments(relative_path));

    if (root.empty()) {
        return normalized_relative.empty() ? "/" : "/" + normalized_relative;
    }

    if (normalized_relative.empty()) {
        return root;
    }

    return root + "/" + normalized_relative;
}

SmbBrowserResult browse_smb_directory(
    const SmbSourceSettings& source,
    const GeneralSettings& general,
    const std::string& relative_path) {
    SmbBrowserResult result;
    result.root_label = smb_source_root_label(source);
    result.requested_path = join_segments(split_relative_segments(relative_path));
    result.resolved_path = smb_display_path(source, result.requested_path);
    result.normalized_extensions = normalize_playable_extensions(general.playable_extensions);

#if defined(_WIN32) && !defined(__SWITCH__)
    result.backend_available = true;

    try {
        const std::string host = trim_network_component(source.host);
        const ParsedShareTarget share_target = parse_share_target(source.share);
        if (host.empty() || share_target.share_name.empty()) {
            result.error_message = "SMB source is missing host or share.";
            return result;
        }

        const std::string effective_relative_path =
            smb_join_relative_path(share_target.initial_relative_path, result.requested_path);

        const std::filesystem::path root_path =
            path_from_utf8("\\\\" + host + "\\" + share_target.share_name);
        const std::filesystem::path browse_path =
            append_relative_segments(root_path, split_relative_segments(effective_relative_path));

        const WindowsSmbAuthResult auth_result = ensure_windows_smb_connection(
            host,
            share_target,
            source.username,
            source.password);

        std::error_code error;
        const bool path_exists = std::filesystem::exists(browse_path, error);

        if (!auth_result.success()) {
            const bool allow_existing_session_fallback =
                auth_result.error_code == ERROR_SESSION_CREDENTIAL_CONFLICT &&
                !error &&
                path_exists;

            if (!allow_existing_session_fallback) {
                result.error_message = auth_result.message;
                return result;
            }
        }

        if (!std::filesystem::exists(browse_path, error)) {
            result.error_message = "SMB path not found: " + path_string(browse_path);
            return result;
        }

        if (!std::filesystem::is_directory(browse_path, error)) {
            result.error_message = "SMB path is not a directory: " + path_string(browse_path);
            return result;
        }

        for (std::filesystem::directory_iterator iterator(
                 browse_path,
                 std::filesystem::directory_options::skip_permission_denied,
                 error);
             !error && iterator != std::filesystem::directory_iterator();
             iterator.increment(error)) {
            const auto& entry = *iterator;
            const std::string name = path_string(entry.path().filename());
            if (name.empty()) {
                continue;
            }

            bool is_directory = entry.is_directory(error);
            if (error) {
                error.clear();
                continue;
            }

            if (is_directory) {
                result.entries.push_back({
                    .name = name,
                    .relative_path = smb_join_relative_path(result.requested_path, name),
                    .is_directory = true,
                    .playable = false,
                    .size = 0,
                });
                continue;
            }

            bool is_regular_file = entry.is_regular_file(error);
            if (error) {
                error.clear();
                continue;
            }

            if (!is_regular_file || !is_playable_extension(name, result.normalized_extensions)) {
                continue;
            }

            const std::uintmax_t file_size = entry.file_size(error);
            if (error) {
                error.clear();
            }

            result.entries.push_back({
                .name = name,
                .relative_path = smb_join_relative_path(result.requested_path, name),
                .is_directory = false,
                .playable = true,
                .size = file_size,
            });
        }

        if (error) {
            result.error_message = "Failed to enumerate SMB directory: " + path_string(browse_path);
            return result;
        }

        std::sort(result.entries.begin(), result.entries.end(), entry_name_less);
        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        result.error_message = std::string("SMB browse exception: ") + exception.what();
        return result;
    } catch (...) {
        result.error_message = "SMB browse exception: unknown error";
        return result;
    }
#else
#ifdef __SWITCH__
    result.backend_available = true;

    try {
        const std::string host = trim_network_component(source.host);
        const ParsedShareTarget share_target = parse_share_target(source.share);
        if (host.empty() || share_target.share_name.empty()) {
            result.error_message = "SMB source is missing host or share.";
            return result;
        }

        std::unique_ptr<smb2_context, Smb2ContextDeleter> smb2(smb2_init_context());
        if (!smb2) {
            result.error_message = "Failed to initialize libsmb2 context.";
            return result;
        }

        smb2_set_timeout(smb2.get(), 10);
        smb2_set_security_mode(smb2.get(), SMB2_NEGOTIATE_SIGNING_ENABLED);

        if (!source.username.empty()) {
            smb2_set_user(smb2.get(), source.username.c_str());
        }

        if (!source.password.empty()) {
            smb2_set_password(smb2.get(), source.password.c_str());
        }

        const int connect_result =
            smb2_connect_share(smb2.get(), host.c_str(), share_target.share_name.c_str(),
                               source.username.empty() ? nullptr : source.username.c_str());
        if (connect_result < 0) {
            result.error_message = smb2_get_error(smb2.get());
            return result;
        }

        const std::string smb2_path = make_smb2_path(share_target, result.requested_path);
        std::unique_ptr<smb2dir, Smb2DirectoryDeleter> directory(
            smb2_opendir(smb2.get(), smb2_path.empty() ? "" : smb2_path.c_str()),
            Smb2DirectoryDeleter{smb2.get()});
        if (!directory) {
            result.error_message = smb2_get_error(smb2.get());
            smb2_disconnect_share(smb2.get());
            return result;
        }

        while (auto* entry = smb2_readdir(smb2.get(), directory.get())) {
            const std::string name = entry->name == nullptr ? "" : entry->name;
            if (name.empty() || name == "." || name == "..") {
                continue;
            }

            const bool is_directory = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
            if (is_directory) {
                result.entries.push_back({
                    .name = name,
                    .relative_path = smb_join_relative_path(result.requested_path, name),
                    .is_directory = true,
                    .playable = false,
                    .size = 0,
                });
                continue;
            }

            if (!is_playable_extension(name, result.normalized_extensions)) {
                continue;
            }

            result.entries.push_back({
                .name = name,
                .relative_path = smb_join_relative_path(result.requested_path, name),
                .is_directory = false,
                .playable = true,
                .size = entry->st.smb2_size,
            });
        }

        directory.reset();
        smb2_disconnect_share(smb2.get());
        std::sort(result.entries.begin(), result.entries.end(), entry_name_less);
        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        result.error_message = std::string("SMB browse exception: ") + exception.what();
        return result;
    } catch (...) {
        result.error_message = "SMB browse exception: unknown error";
        return result;
    }
#else
    (void)source;
    (void)general;
    result.backend_available = false;
    result.error_message = "SMB browser backend is not available on this platform yet.";
    return result;
#endif
#endif
}

bool delete_smb_file(
    const SmbSourceSettings& source,
    const std::string& relative_path,
    std::string& error_message) {
    error_message.clear();
    const std::string normalized_relative_path = join_segments(split_relative_segments(relative_path));
    if (normalized_relative_path.empty()) {
        error_message = "SMB file path is empty.";
        return false;
    }

#if defined(_WIN32) && !defined(__SWITCH__)
    try {
        const std::string host = trim_network_component(source.host);
        const ParsedShareTarget share_target = parse_share_target(source.share);
        if (host.empty() || share_target.share_name.empty()) {
            error_message = "SMB source is missing host or share.";
            return false;
        }

        const WindowsSmbAuthResult auth_result = ensure_windows_smb_connection(
            host,
            share_target,
            source.username,
            source.password);
        if (!auth_result.success() &&
            auth_result.error_code != ERROR_SESSION_CREDENTIAL_CONFLICT) {
            error_message = auth_result.message;
            return false;
        }

        const std::string effective_relative_path =
            smb_join_relative_path(share_target.initial_relative_path, normalized_relative_path);
        const std::filesystem::path root_path =
            path_from_utf8("\\\\" + host + "\\" + share_target.share_name);
        const std::filesystem::path target_path =
            append_relative_segments(root_path, split_relative_segments(effective_relative_path));

        std::error_code remove_error;
        const std::uintmax_t removed_count = std::filesystem::remove_all(target_path, remove_error);
        if (remove_error || removed_count == 0) {
            error_message = "Failed to delete SMB file: " + path_string(target_path);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error_message = std::string("SMB delete exception: ") + exception.what();
        return false;
    } catch (...) {
        error_message = "SMB delete exception: unknown error";
        return false;
    }
#else
#ifdef __SWITCH__
    try {
        const std::string host = trim_network_component(source.host);
        const ParsedShareTarget share_target = parse_share_target(source.share);
        if (host.empty() || share_target.share_name.empty()) {
            error_message = "SMB source is missing host or share.";
            return false;
        }

        std::unique_ptr<smb2_context, Smb2ContextDeleter> smb2(smb2_init_context());
        if (!smb2) {
            error_message = "Failed to initialize libsmb2 context.";
            return false;
        }

        smb2_set_timeout(smb2.get(), 10);
        smb2_set_security_mode(smb2.get(), SMB2_NEGOTIATE_SIGNING_ENABLED);
        if (!source.username.empty()) {
            smb2_set_user(smb2.get(), source.username.c_str());
        }
        if (!source.password.empty()) {
            smb2_set_password(smb2.get(), source.password.c_str());
        }

        const int connect_result =
            smb2_connect_share(
                smb2.get(),
                host.c_str(),
                share_target.share_name.c_str(),
                source.username.empty() ? nullptr : source.username.c_str());
        if (connect_result < 0) {
            error_message = smb2_get_error(smb2.get());
            return false;
        }

        const std::string smb2_path = make_smb2_path(share_target, normalized_relative_path);
        if (smb2_path.empty()) {
            smb2_disconnect_share(smb2.get());
            error_message = "SMB file path is empty.";
            return false;
        }

        if (!delete_smb_path_recursive(smb2.get(), smb2_path, error_message)) {
            smb2_disconnect_share(smb2.get());
            return false;
        }

        smb2_disconnect_share(smb2.get());
        return true;
    } catch (const std::exception& exception) {
        error_message = std::string("SMB delete exception: ") + exception.what();
        return false;
    } catch (...) {
        error_message = "SMB delete exception: unknown error";
        return false;
    }
#else
    (void)source;
    (void)relative_path;
    error_message = "SMB delete backend is not available on this platform.";
    return false;
#endif
#endif
}

}  // namespace switchbox::core
