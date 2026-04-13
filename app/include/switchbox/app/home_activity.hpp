#pragma once

#include <borealis.hpp>

#include "switchbox/app/startup_context.hpp"

namespace switchbox::app {

class HomeActivity : public brls::Activity {
public:
    explicit HomeActivity(const StartupContext& context);
};

}  // namespace switchbox::app
