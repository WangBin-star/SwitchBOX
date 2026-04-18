#pragma once

#include <memory>

#include <borealis.hpp>

#include "switchbox/app/startup_context.hpp"
#include "switchbox/core/app_config.hpp"

namespace switchbox::app {

class HomeActivity : public brls::Activity {
public:
    explicit HomeActivity(const StartupContext& context);
    ~HomeActivity() override;
    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    struct IptvLoadState;

    brls::View* build_content();
    void start_iptv_loading(const switchbox::core::IptvSourceSettings& source);
    void cancel_iptv_loading(bool user_cancelled);
    void refresh_iptv_loading_overlay();
    void handle_iptv_loading_completion();
    void hide_iptv_loading_overlay(bool restore_focus);
    void restore_home_focus();

    brls::Box* iptv_loading_overlay = nullptr;
    brls::Label* iptv_loading_source_label = nullptr;
    brls::Label* iptv_loading_detail_label = nullptr;
    brls::Label* iptv_loading_percent_label = nullptr;
    brls::Box* iptv_loading_fill_view = nullptr;
    brls::RepeatingTimer iptv_loading_timer;
    std::shared_ptr<IptvLoadState> iptv_load_state;
    switchbox::core::IptvSourceSettings pending_iptv_source;
    brls::View* home_focus_before_iptv_loading = nullptr;
    brls::ActionIdentifier cancel_loading_action_id = ACTION_NONE;
    bool iptv_loading_visible = false;
    bool iptv_loading_completion_handled = false;
};

}  // namespace switchbox::app
