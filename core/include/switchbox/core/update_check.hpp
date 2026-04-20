#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace switchbox::core {

struct UpdateCheckResult {
    bool backend_available = false;
    bool success = false;
    std::string latest_version;
    std::string release_url;
    std::string error_message;
};

UpdateCheckResult fetch_latest_release_version(
    const std::shared_ptr<std::atomic_bool>& cancel_flag = {});

}  // namespace switchbox::core
