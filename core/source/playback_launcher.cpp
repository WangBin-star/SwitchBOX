#include "switchbox/core/playback_launcher.hpp"
#include "switchbox/core/switch_mpv_player.hpp"

namespace switchbox::core {

PlaybackLaunchResult launch_playback(const PlaybackTarget& target) {
    PlaybackLaunchResult result;
    result.backend_available = switch_mpv_backend_available();
    if (!result.backend_available) {
        result.error_message = switch_mpv_backend_reason();
        return result;
    }

    if (switch_mpv_open(target, result.error_message)) {
        result.started = true;
        result.attempted_locator =
            target.primary_locator.empty() ? target.fallback_locator : target.primary_locator;
        return result;
    }

    if (result.error_message.empty()) {
        result.error_message = "Unable to start switch playback backend.";
    }
    return result;
}

}  // namespace switchbox::core
