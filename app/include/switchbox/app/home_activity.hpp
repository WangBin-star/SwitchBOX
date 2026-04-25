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
    void onResume() override;

private:
    struct IptvLoadState;
    struct WebDavLoadState;
    enum class LoadingKind {
        None,
        Iptv,
        WebDav,
    };

    brls::View* build_content();
    void start_iptv_loading(const switchbox::core::IptvSourceSettings& source);
    void start_webdav_loading(const switchbox::core::WebDavSourceSettings& source);
    void cancel_iptv_loading(bool user_cancelled);
    void cancel_webdav_loading(bool user_cancelled);
    void refresh_iptv_loading_overlay();
    void refresh_webdav_loading_overlay();
    void handle_iptv_loading_completion();
    void handle_webdav_loading_completion();
    void hide_loading_overlay(bool restore_focus);
    void restore_home_focus();

    brls::Box* loading_overlay = nullptr;
    brls::Label* loading_title_label = nullptr;
    brls::Label* loading_source_label = nullptr;
    brls::Label* loading_detail_label = nullptr;
    brls::Label* loading_percent_label = nullptr;
    brls::Box* loading_fill_view = nullptr;
    brls::RepeatingTimer loading_timer;
    std::shared_ptr<IptvLoadState> iptv_load_state;
    std::shared_ptr<WebDavLoadState> webdav_load_state;
    switchbox::core::IptvSourceSettings pending_iptv_source;
    switchbox::core::WebDavSourceSettings pending_webdav_source;
    brls::View* home_focus_before_iptv_loading = nullptr;
    brls::ActionIdentifier cancel_loading_action_id = ACTION_NONE;
    LoadingKind active_loading_kind = LoadingKind::None;
    bool loading_visible = false;
    bool iptv_loading_completion_handled = false;
    bool webdav_loading_completion_handled = false;
};

}  // namespace switchbox::app
