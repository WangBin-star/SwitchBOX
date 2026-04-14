#pragma once

#include <borealis.hpp>

#include "switchbox/core/playback_target.hpp"

namespace switchbox::app {

class PlayerActivity : public brls::Activity {
public:
    explicit PlayerActivity(switchbox::core::PlaybackTarget target);
    ~PlayerActivity() override;

private:
    bool handle_start_action(brls::View* view);

    switchbox::core::PlaybackTarget target;
};

}  // namespace switchbox::app
