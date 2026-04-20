#pragma once

#include <borealis.hpp>
#include <memory>

namespace switchbox::app {

struct SettingsDraftState;

class SettingsActivity : public brls::Activity {
public:
    SettingsActivity();
    ~SettingsActivity() override;
    brls::View* createContentView() override;
    void willAppear(bool resetState = false) override;

private:
    std::shared_ptr<SettingsDraftState> state;
};

}  // namespace switchbox::app
