#pragma once

#include "switchbox/app/startup_context.hpp"

namespace switchbox::app {

class Application {
public:
    static void set_runtime_context(const StartupContext& context);
    static const StartupContext& runtime_context();
    static void reload_root_ui(bool reopen_settings = false);
    static void apply_language_and_reload_ui(bool reopen_settings = false);

    int run(const StartupContext& context) const;
};

}  // namespace switchbox::app
