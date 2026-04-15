#pragma once

#include <string>

#include "switchbox/core/playback_target.hpp"

namespace switchbox::core {

bool switch_smb_mount_resolve_playback_path(
    const PlaybackTarget::SmbLocator& locator,
    std::string& mounted_path,
    std::string& error_message);

void switch_smb_mount_release();

}  // namespace switchbox::core
