#include "switchbox/core/smb_browser.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>

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

std::string path_string(const std::filesystem::path& path) {
    const auto native = path.generic_u8string();
    return std::string(native.begin(), native.end());
}

std::filesystem::path path_from_utf8(const std::string& value) {
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

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

bool entry_name_greater(const SmbBrowserEntry& lhs, const SmbBrowserEntry& rhs) {
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory && !rhs.is_directory;
    }

    return to_lower(lhs.name) > to_lower(rhs.name);
}

template <typename ValueGetter>
bool entry_value_less(
    const SmbBrowserEntry& lhs,
    const SmbBrowserEntry& rhs,
    const ValueGetter& value_getter,
    bool ascending) {
    if (lhs.is_directory != rhs.is_directory) {
        return lhs.is_directory && !rhs.is_directory;
    }

    const auto lhs_value = value_getter(lhs);
    const auto rhs_value = value_getter(rhs);
    if (lhs_value != rhs_value) {
        return ascending ? (lhs_value < rhs_value) : (lhs_value > rhs_value);
    }

    const std::string lhs_name = to_lower(lhs.name);
    const std::string rhs_name = to_lower(rhs.name);
    if (lhs_name != rhs_name) {
        return ascending ? (lhs_name < rhs_name) : (lhs_name > rhs_name);
    }

    return lhs.relative_path < rhs.relative_path;
}

void sort_entries(
    std::vector<SmbBrowserEntry>& entries,
    const std::string& sort_order) {
    if (sort_order == "name_desc") {
        std::sort(entries.begin(), entries.end(), entry_name_greater);
        return;
    }

    if (sort_order == "date_asc") {
        std::sort(entries.begin(), entries.end(), [](const SmbBrowserEntry& lhs, const SmbBrowserEntry& rhs) {
            return entry_value_less(
                lhs,
                rhs,
                [](const SmbBrowserEntry& entry) {
                    return entry.modified_timestamp;
                },
                true);
        });
        return;
    }

    if (sort_order == "date_desc") {
        std::sort(entries.begin(), entries.end(), [](const SmbBrowserEntry& lhs, const SmbBrowserEntry& rhs) {
            return entry_value_less(
                lhs,
                rhs,
                [](const SmbBrowserEntry& entry) {
                    return entry.modified_timestamp;
                },
                false);
        });
        return;
    }

    if (sort_order == "size_asc") {
        std::sort(entries.begin(), entries.end(), [](const SmbBrowserEntry& lhs, const SmbBrowserEntry& rhs) {
            return entry_value_less(
                lhs,
                rhs,
                [](const SmbBrowserEntry& entry) {
                    return static_cast<std::uintmax_t>(entry.is_directory ? 0 : entry.size);
                },
                true);
        });
        return;
    }

    if (sort_order == "size_desc") {
        std::sort(entries.begin(), entries.end(), [](const SmbBrowserEntry& lhs, const SmbBrowserEntry& rhs) {
            return entry_value_less(
                lhs,
                rhs,
                [](const SmbBrowserEntry& entry) {
                    return static_cast<std::uintmax_t>(entry.is_directory ? 0 : entry.size);
                },
                false);
        });
        return;
    }

    std::sort(entries.begin(), entries.end(), entry_name_less);
}

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
                    .modified_timestamp = static_cast<std::int64_t>(entry->st.smb2_mtime),
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
                .modified_timestamp = static_cast<std::int64_t>(entry->st.smb2_mtime),
            });
        }

        directory.reset();
        smb2_disconnect_share(smb2.get());
        sort_entries(result.entries, general.sort_order);
        result.success = true;
        return result;
    } catch (const std::exception& exception) {
        result.error_message = std::string("SMB browse exception: ") + exception.what();
        return result;
    } catch (...) {
        result.error_message = "SMB browse exception: unknown error";
        return result;
    }
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

        constexpr int kDeleteRetryCount = 10;
        for (int attempt = 0; attempt < kDeleteRetryCount; ++attempt) {
            error_message.clear();
            if (delete_smb_path_recursive(smb2.get(), smb2_path, error_message)) {
                smb2_disconnect_share(smb2.get());
                return true;
            }

            if (attempt + 1 < kDeleteRetryCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        smb2_disconnect_share(smb2.get());
        return false;
    } catch (const std::exception& exception) {
        error_message = std::string("SMB delete exception: ") + exception.what();
        return false;
    } catch (...) {
        error_message = "SMB delete exception: unknown error";
        return false;
    }
}

}  // namespace switchbox::core
