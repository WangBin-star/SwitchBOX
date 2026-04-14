#pragma once

#include <string>

#include "switchbox/core/playback_target.hpp"

namespace switchbox::core {

struct PlaybackLaunchResult {
    bool backend_available = false;
    bool started = false;
    bool used_fallback_locator = false;
    std::string attempted_locator;
    std::string error_message;
};

PlaybackLaunchResult launch_playback(const PlaybackTarget& target);

}  // namespace switchbox::core
