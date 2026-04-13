#pragma once

#include "switchbox/app/startup_context.hpp"

namespace switchbox::app {

class Application {
public:
    int run(const StartupContext& context) const;
};

}  // namespace switchbox::app
