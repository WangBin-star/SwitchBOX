#include "switchbox/core/smb2_mount_fs.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __SWITCH__
#include <fcntl.h>
#include <sys/dirent.h>
#include <sys/iosupport.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <smb2/libsmb2.h>
#include <smb2/smb2.h>
#include <switch.h>
#endif

namespace switchbox::core {

#ifdef __SWITCH__

namespace {

constexpr const char* kDeviceName = "smb0";
constexpr const char* kMountName = "smb0:";

struct ParsedShareTarget {
    std::string share_name;
    std::string base_relative_path;
};

struct MountConfig {
    std::string host;
    std::string share_name;
    std::string base_relative_path;
    std::string username;
    std::string password;

    bool operator==(const MountConfig&) const = default;
};

struct Smb2ContextDeleter {
    void operator()(smb2_context* context) const {
        if (context != nullptr) {
            smb2_destroy_context(context);
        }
    }
};

struct SmbDirectoryCacheEntry {
    std::string name;
    struct stat metadata {};
};

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
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
    std::string joined;
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
            joined.push_back('/');
        }
        joined += segments[index];
    }
    return joined;
}

ParsedShareTarget parse_share_target(const std::string& raw_share) {
    const auto segments = split_relative_segments(raw_share);
    if (segments.empty()) {
        return {};
    }

    ParsedShareTarget parsed;
    parsed.share_name = segments.front();
    if (segments.size() > 1) {
        parsed.base_relative_path =
            join_segments(std::vector<std::string>(segments.begin() + 1, segments.end()));
    }
    return parsed;
}

MountConfig make_mount_config(
    const PlaybackTarget::SmbLocator& locator,
    std::string& error_message) {
    error_message.clear();

    const std::string host = trim_network_component(locator.host);
    const ParsedShareTarget share_target = parse_share_target(locator.share);
    if (host.empty() || share_target.share_name.empty()) {
        error_message = "SMB playback target is missing host/share/path.";
        return {};
    }

    return {
        .host = host,
        .share_name = share_target.share_name,
        .base_relative_path = share_target.base_relative_path,
        .username = locator.username,
        .password = locator.password,
    };
}

std::string make_virtual_playback_path(const std::string& relative_path) {
    const std::string normalized_relative = join_segments(split_relative_segments(relative_path));
    if (normalized_relative.empty()) {
        return {};
    }

    return std::string(kMountName) + "/" + normalized_relative;
}

void fill_stat_from_smb2(const smb2_stat_64& source, struct stat* destination) {
    if (destination == nullptr) {
        return;
    }

    *destination = {};
    destination->st_mode = source.smb2_type == SMB2_TYPE_DIRECTORY ? S_IFDIR : S_IFREG;
    destination->st_nlink = 1;
    destination->st_uid = 1;
    destination->st_gid = 2;
    destination->st_size = static_cast<off_t>(source.smb2_size);
    destination->st_atime = static_cast<time_t>(source.smb2_atime);
    destination->st_mtime = static_cast<time_t>(source.smb2_mtime);
    destination->st_ctime = static_cast<time_t>(source.smb2_ctime);
}

void fill_virtual_root_stat(struct stat* destination) {
    if (destination == nullptr) {
        return;
    }

    *destination = {};
    destination->st_mode = S_IFDIR;
    destination->st_nlink = 1;
    destination->st_uid = 1;
    destination->st_gid = 2;
}

class MountedSmbDevice {
public:
    explicit MountedSmbDevice(MountConfig config)
        : config_(std::move(config)),
          base_segments_(split_relative_segments(config_.base_relative_path)) {
        this->devoptab_ = {};
        this->devoptab_.name = kDeviceName;
        this->devoptab_.structSize = sizeof(FileHandle);
        this->devoptab_.open_r = &MountedSmbDevice::device_open;
        this->devoptab_.close_r = &MountedSmbDevice::device_close;
        this->devoptab_.read_r = &MountedSmbDevice::device_read;
        this->devoptab_.seek_r = &MountedSmbDevice::device_seek;
        this->devoptab_.fstat_r = &MountedSmbDevice::device_fstat;
        this->devoptab_.stat_r = &MountedSmbDevice::device_stat;
        this->devoptab_.chdir_r = &MountedSmbDevice::device_chdir;
        this->devoptab_.dirStateSize = sizeof(DirectoryHandle);
        this->devoptab_.diropen_r = &MountedSmbDevice::device_diropen;
        this->devoptab_.dirreset_r = &MountedSmbDevice::device_dirreset;
        this->devoptab_.dirnext_r = &MountedSmbDevice::device_dirnext;
        this->devoptab_.dirclose_r = &MountedSmbDevice::device_dirclose;
        this->devoptab_.statvfs_r = &MountedSmbDevice::device_statvfs;
        this->devoptab_.lstat_r = &MountedSmbDevice::device_lstat;
        this->devoptab_.deviceData = this;
    }

    ~MountedSmbDevice() {
        unmount();
    }

    bool mount(std::string& error_message) {
        error_message.clear();

        std::unique_ptr<smb2_context, Smb2ContextDeleter> smb2(smb2_init_context());
        if (!smb2) {
            error_message = "Failed to initialize libsmb2 context.";
            return false;
        }

        smb2_set_timeout(smb2.get(), 10);
        smb2_set_security_mode(smb2.get(), SMB2_NEGOTIATE_SIGNING_ENABLED);
        if (!config_.username.empty()) {
            smb2_set_user(smb2.get(), config_.username.c_str());
        }
        if (!config_.password.empty()) {
            smb2_set_password(smb2.get(), config_.password.c_str());
        }

        const int connect_result = smb2_connect_share(
            smb2.get(),
            config_.host.c_str(),
            config_.share_name.c_str(),
            config_.username.empty() ? nullptr : config_.username.c_str());
        if (connect_result < 0) {
            error_message = smb2_get_error(smb2.get());
            return false;
        }

        this->smb2_ = std::move(smb2);
        this->share_connected_ = true;

        if (FindDevice(kMountName) >= 0) {
            RemoveDevice(kMountName);
        }

        const int add_result = AddDevice(&this->devoptab_);
        if (add_result < 0) {
            error_message = "Failed to register SMB playback device.";
            unmount();
            return false;
        }

        this->registered_ = true;
        this->cwd_ = "/";
        return true;
    }

    void unmount() {
        std::scoped_lock lock(this->session_mutex_);

        if (this->registered_) {
            RemoveDevice(kMountName);
            this->registered_ = false;
        }

        if (this->smb2_ && this->share_connected_) {
            smb2_disconnect_share(this->smb2_.get());
            this->share_connected_ = false;
        }

        this->smb2_.reset();
        this->cwd_ = "/";
    }

private:
    struct FileHandle {
        smb2fh* handle = nullptr;
        smb2_stat_64 metadata {};
        off_t offset = 0;
    };

    struct DirectoryHandle {
        size_t index = 0;
    };

    std::vector<std::string> make_virtual_segments(const char* path) const {
        std::string raw = path == nullptr ? "" : path;
        if (raw.rfind(kMountName, 0) == 0) {
            raw.erase(0, std::strlen(kMountName));
        }

        bool is_absolute = false;
        while (!raw.empty() && (raw.front() == '/' || raw.front() == '\\')) {
            raw.erase(raw.begin());
            is_absolute = true;
        }

        std::vector<std::string> segments;
        if (!is_absolute) {
            segments = split_relative_segments(this->cwd_);
        }

        const auto input_segments = split_relative_segments(raw);
        segments.insert(segments.end(), input_segments.begin(), input_segments.end());
        return segments;
    }

    std::string normalize_virtual_directory(const char* path) const {
        const auto segments = make_virtual_segments(path);
        const std::string joined = join_segments(segments);
        return joined.empty() ? "/" : "/" + joined;
    }

    std::string resolve_smb_relative_path(const char* path) const {
        std::vector<std::string> segments = base_segments_;
        const auto virtual_segments = make_virtual_segments(path);
        segments.insert(segments.end(), virtual_segments.begin(), virtual_segments.end());
        return join_segments(segments);
    }

    std::string base_root_relative_path() const {
        return join_segments(base_segments_);
    }

    static int set_reent_error(struct _reent* reent, int error_code) {
        __errno_r(reent) = error_code;
        return -1;
    }

    static int device_open(struct _reent* reent, void* file_struct, const char* path, int flags, int mode) {
        (void)mode;
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        auto* file = static_cast<FileHandle*>(file_struct);
        if (self == nullptr || file == nullptr) {
            return set_reent_error(reent, EIO);
        }

        if ((flags & O_ACCMODE) != O_RDONLY) {
            return set_reent_error(reent, EACCES);
        }

        const std::string smb_path = self->resolve_smb_relative_path(path);
        if (smb_path.empty()) {
            return set_reent_error(reent, EISDIR);
        }

        std::scoped_lock lock(self->session_mutex_);
        smb2fh* handle = smb2_open(self->smb2_.get(), smb_path.c_str(), O_RDONLY);
        if (handle == nullptr) {
            return set_reent_error(reent, EIO);
        }

        file->handle = handle;
        if (smb2_stat(self->smb2_.get(), smb_path.c_str(), &file->metadata) < 0) {
            smb2_close(self->smb2_.get(), handle);
            file->handle = nullptr;
            return set_reent_error(reent, EIO);
        }

        file->offset = 0;
        return 0;
    }

    static int device_close(struct _reent* reent, void* fd) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        auto* file = static_cast<FileHandle*>(fd);
        if (self == nullptr || file == nullptr || file->handle == nullptr) {
            return set_reent_error(reent, EIO);
        }

        std::scoped_lock lock(self->session_mutex_);
        const int result = smb2_close(self->smb2_.get(), file->handle);
        file->handle = nullptr;
        if (result < 0) {
            return set_reent_error(reent, EIO);
        }

        return 0;
    }

    static ssize_t device_read(struct _reent* reent, void* fd, char* ptr, size_t len) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        auto* file = static_cast<FileHandle*>(fd);
        if (self == nullptr || file == nullptr || file->handle == nullptr || ptr == nullptr) {
            return set_reent_error(reent, EIO);
        }

        std::scoped_lock lock(self->session_mutex_);
        const auto result = smb2_pread(
            self->smb2_.get(),
            file->handle,
            reinterpret_cast<std::uint8_t*>(ptr),
            len,
            file->offset);
        if (result < 0) {
            set_reent_error(reent, EIO);
            return -1;
        }

        file->offset += static_cast<off_t>(result);
        return result;
    }

    static off_t device_seek(struct _reent* reent, void* fd, off_t pos, int dir) {
        auto* file = static_cast<FileHandle*>(fd);
        if (file == nullptr) {
            set_reent_error(reent, EIO);
            return -1;
        }

        off_t base_offset = 0;
        switch (dir) {
            case SEEK_SET:
                base_offset = 0;
                break;
            case SEEK_CUR:
                base_offset = file->offset;
                break;
            case SEEK_END:
                base_offset = static_cast<off_t>(file->metadata.smb2_size);
                break;
            default:
                set_reent_error(reent, EINVAL);
                return -1;
        }

        const off_t next_offset = base_offset + pos;
        if (next_offset < 0) {
            set_reent_error(reent, EINVAL);
            return -1;
        }

        file->offset = next_offset;
        return next_offset;
    }

    static int device_fstat(struct _reent* reent, void* fd, struct stat* st) {
        auto* file = static_cast<FileHandle*>(fd);
        if (file == nullptr || st == nullptr) {
            return set_reent_error(reent, EIO);
        }

        fill_stat_from_smb2(file->metadata, st);
        return 0;
    }

    static int device_stat_common(struct _reent* reent, const char* path, struct stat* st) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        if (self == nullptr || st == nullptr) {
            return set_reent_error(reent, EIO);
        }

        const std::string smb_path = self->resolve_smb_relative_path(path);
        if (smb_path.empty()) {
            const std::string root_path = self->base_root_relative_path();
            if (root_path.empty()) {
                fill_virtual_root_stat(st);
                return 0;
            }

            smb2_stat_64 smb_stat {};
            std::scoped_lock lock(self->session_mutex_);
            if (smb2_stat(self->smb2_.get(), root_path.c_str(), &smb_stat) < 0) {
                return set_reent_error(reent, EIO);
            }

            fill_stat_from_smb2(smb_stat, st);
            return 0;
        }

        smb2_stat_64 smb_stat {};
        std::scoped_lock lock(self->session_mutex_);
        if (smb2_stat(self->smb2_.get(), smb_path.c_str(), &smb_stat) < 0) {
            return set_reent_error(reent, EIO);
        }

        fill_stat_from_smb2(smb_stat, st);
        return 0;
    }

    static int device_stat(struct _reent* reent, const char* file, struct stat* st) {
        return device_stat_common(reent, file, st);
    }

    static int device_lstat(struct _reent* reent, const char* file, struct stat* st) {
        return device_stat_common(reent, file, st);
    }

    static int device_chdir(struct _reent* reent, const char* name) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        if (self == nullptr || name == nullptr) {
            return set_reent_error(reent, EINVAL);
        }

        self->cwd_ = self->normalize_virtual_directory(name);
        return 0;
    }

    static DIR_ITER* device_diropen(struct _reent* reent, DIR_ITER* dir_state, const char* path) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        auto* dir = dir_state == nullptr ? nullptr : static_cast<DirectoryHandle*>(dir_state->dirStruct);
        if (self == nullptr || dir == nullptr) {
            set_reent_error(reent, EIO);
            return nullptr;
        }

        const std::string smb_path = self->resolve_smb_relative_path(path);
        {
            std::scoped_lock lock(self->session_mutex_);
            self->cached_entries_.clear();
            smb2dir* directory = smb2_opendir(
                self->smb2_.get(),
                smb_path.empty() ? "" : smb_path.c_str());
            if (directory == nullptr) {
                set_reent_error(reent, EIO);
                return nullptr;
            }

            while (auto* entry = smb2_readdir(self->smb2_.get(), directory)) {
                const std::string name = entry->name == nullptr ? "" : entry->name;
                if (name.empty() || name == "." || name == "..") {
                    continue;
                }

                SmbDirectoryCacheEntry cached_entry;
                cached_entry.name = name;
                fill_stat_from_smb2(entry->st, &cached_entry.metadata);
                self->cached_entries_.push_back(std::move(cached_entry));
            }

            smb2_closedir(self->smb2_.get(), directory);
        }

        dir->index = 0;
        return dir_state;
    }

    static int device_dirreset(struct _reent* reent, DIR_ITER* dir_state) {
        auto* dir = dir_state == nullptr ? nullptr : static_cast<DirectoryHandle*>(dir_state->dirStruct);
        if (dir == nullptr) {
            return set_reent_error(reent, EIO);
        }

        dir->index = 0;
        return 0;
    }

    static int device_dirnext(struct _reent* reent, DIR_ITER* dir_state, char* filename, struct stat* filestat) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        auto* dir = dir_state == nullptr ? nullptr : static_cast<DirectoryHandle*>(dir_state->dirStruct);
        if (self == nullptr || dir == nullptr || filename == nullptr || filestat == nullptr) {
            return set_reent_error(reent, EIO);
        }

        if (dir->index >= self->cached_entries_.size()) {
            return set_reent_error(reent, ENOENT);
        }

        const auto& entry = self->cached_entries_[dir->index++];
        std::memset(filename, 0, NAME_MAX);
        std::strncpy(filename, entry.name.c_str(), NAME_MAX - 1);
        *filestat = entry.metadata;
        return 0;
    }

    static int device_dirclose(struct _reent* reent, DIR_ITER* dir_state) {
        auto* dir = dir_state == nullptr ? nullptr : static_cast<DirectoryHandle*>(dir_state->dirStruct);
        if (dir == nullptr) {
            return set_reent_error(reent, EIO);
        }

        dir->index = 0;
        return 0;
    }

    static int device_statvfs(struct _reent* reent, const char* path, struct statvfs* buf) {
        auto* self = static_cast<MountedSmbDevice*>(reent->deviceData);
        if (self == nullptr || buf == nullptr) {
            return set_reent_error(reent, EIO);
        }

        const std::string smb_path = self->resolve_smb_relative_path(path);
        const std::string statvfs_path = smb_path.empty() ? "/" : "/" + smb_path;

        struct smb2_statvfs info {};
        std::scoped_lock lock(self->session_mutex_);
        if (smb2_statvfs(self->smb2_.get(), statvfs_path.c_str(), &info) < 0) {
            return set_reent_error(reent, EIO);
        }

        *buf = {};
        buf->f_bsize = info.f_bsize;
        buf->f_frsize = info.f_frsize;
        buf->f_blocks = info.f_blocks;
        buf->f_bfree = info.f_bfree;
        buf->f_bavail = info.f_bavail;
        buf->f_files = info.f_files;
        buf->f_ffree = info.f_ffree;
        buf->f_favail = info.f_favail;
        buf->f_fsid = info.f_fsid;
        buf->f_flag = info.f_flag;
        buf->f_namemax = info.f_namemax;
        return 0;
    }

    MountConfig config_;
    std::vector<std::string> base_segments_;
    std::string cwd_ = "/";
    devoptab_t devoptab_ {};
    std::unique_ptr<smb2_context, Smb2ContextDeleter> smb2_;
    bool share_connected_ = false;
    bool registered_ = false;
    std::vector<SmbDirectoryCacheEntry> cached_entries_;
    mutable std::mutex session_mutex_;
};

class SmbMountManager {
public:
    bool resolve_playback_path(
        const PlaybackTarget::SmbLocator& locator,
        std::string& mounted_path,
        std::string& error_message) {
        MountConfig config = make_mount_config(locator, error_message);
        if (!error_message.empty()) {
            return false;
        }

        if (!this->mounted_config_.has_value() || this->mounted_config_.value() != config) {
            this->device_.reset();
            auto device = std::make_unique<MountedSmbDevice>(config);
            if (!device->mount(error_message)) {
                return false;
            }

            this->mounted_config_ = std::move(config);
            this->device_ = std::move(device);
        }

        mounted_path = make_virtual_playback_path(locator.relative_path);
        if (mounted_path.empty()) {
            error_message = "SMB playback target is missing file path.";
            return false;
        }

        return true;
    }

    void release() {
        this->device_.reset();
        this->mounted_config_.reset();
    }

private:
    std::optional<MountConfig> mounted_config_;
    std::unique_ptr<MountedSmbDevice> device_;
};

SmbMountManager& smb_mount_manager() {
    static SmbMountManager manager;
    return manager;
}

}  // namespace

bool switch_smb_mount_resolve_playback_path(
    const PlaybackTarget::SmbLocator& locator,
    std::string& mounted_path,
    std::string& error_message) {
    return smb_mount_manager().resolve_playback_path(locator, mounted_path, error_message);
}

void switch_smb_mount_release() {
    smb_mount_manager().release();
}

#else

bool switch_smb_mount_resolve_playback_path(
    const PlaybackTarget::SmbLocator&,
    std::string&,
    std::string& error_message) {
    error_message = "SMB playback mount is only available on Switch.";
    return false;
}

void switch_smb_mount_release() {
}

#endif

}  // namespace switchbox::core
