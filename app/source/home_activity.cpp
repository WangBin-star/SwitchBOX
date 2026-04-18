#include "switchbox/app/home_activity.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <borealis/core/i18n.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/h_scrolling_frame.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/rectangle.hpp>

#include "switchbox/app/iptv_browser_activity.hpp"
#include "switchbox/app/placeholder_activity.hpp"
#include "switchbox/app/settings_activity.hpp"
#include "switchbox/app/smb_browser_activity.hpp"
#include "switchbox/core/app_config.hpp"

namespace switchbox::app {

namespace {

std::string tr(const std::string& key) {
    return brls::getStr("switchbox/" + key);
}

brls::Label* create_label(
    const std::string& text,
    float font_size,
    NVGcolor color,
    bool single_line = false) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(font_size);
    label->setTextColor(color);
    label->setSingleLine(single_line);
    return label;
}

NVGcolor with_alpha(NVGcolor color, float alpha) {
    return nvgRGBAf(color.r, color.g, color.b, alpha);
}

std::filesystem::path iptv_debug_log_path() {
    return switchbox::core::AppConfigStore::paths().base_directory / "iptv_debug.log";
}

void append_home_iptv_debug_log(const std::string& message) {
    std::ofstream output(iptv_debug_log_path(), std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return;
    }

    std::string sanitized = message;
    for (char& character : sanitized) {
        if (character == '\r' || character == '\n') {
            character = ' ';
        }
    }

    output << "[home-iptv] " << sanitized << '\n';
}

std::string sanitize_home_log_text(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n') {
            character = ' ';
        }
    }
    return value;
}

std::string summarize_iptv_source(const switchbox::core::IptvSourceSettings& source) {
    if (!source.url.empty()) {
        return source.url;
    }

    return tr("home/cards/common/not_set");
}

std::string summarize_smb_source(const switchbox::core::SmbSourceSettings& source) {
    if (!source.host.empty() && !source.share.empty()) {
        return source.host + "/" + source.share;
    }

    if (!source.host.empty()) {
        return source.host;
    }

    if (!source.share.empty()) {
        return source.share;
    }

    return tr("home/cards/common/not_set");
}

std::string visible_iptv_title(const switchbox::core::IptvSourceSettings& source) {
    if (!source.title.empty()) {
        return source.title;
    }

    return tr("home/cards/common/untitled_iptv");
}

std::string visible_smb_title(const switchbox::core::SmbSourceSettings& source) {
    if (!source.title.empty()) {
        return source.title;
    }

    return tr("home/cards/common/untitled_smb");
}

PlaceholderSection make_history_section() {
    return {
        .title = tr("sections/history/title"),
        .subtitle = tr("sections/history/subtitle"),
        .checkpoints =
            {
                tr("sections/history/checkpoints/1"),
                tr("sections/history/checkpoints/2"),
                tr("sections/history/checkpoints/3"),
            },
    };
}

struct HomeCardModel {
    std::string eyebrow;
    std::string title;
    std::string subtitle;
    std::string footer;
    NVGcolor accent_color = nvgRGB(255, 255, 255);
    std::function<void()> action;
};

class HomeSourceCard : public brls::Box {
public:
    explicit HomeSourceCard(HomeCardModel model)
        : brls::Box(brls::Axis::COLUMN)
        , model(std::move(model)) {
        setFocusable(true);
        setDimensions(344, 420);
        setMargins(0, 22, 0, 22);
        setPadding(32, 30, 30, 30);
        setCornerRadius(30.0f);
        setHighlightCornerRadius(34.0f);
        setHideHighlightBackground(true);
        setShadowType(brls::ShadowType::GENERIC);
        setBorderThickness(1.0f);

        auto* top = new brls::Box(brls::Axis::COLUMN);
        top->setGrow(1.0f);
        top->setJustifyContent(brls::JustifyContent::FLEX_START);

        eyebrow_label = create_label(this->model.eyebrow, 16.0f, this->model.accent_color, true);
        eyebrow_label->setMargins(0, 0, 18, 0);
        top->addView(eyebrow_label);

        title_label = create_label(this->model.title, 30.0f, brls::Application::getTheme()["brls/text"], true);
        title_label->setMargins(0, 0, 14, 0);
        top->addView(title_label);

        subtitle_label = create_label(
            this->model.subtitle,
            18.0f,
            brls::Application::getTheme()["brls/header/subtitle"],
            true);
        subtitle_label->setMargins(0, 0, 12, 0);
        top->addView(subtitle_label);

        addView(top);

        footer_label = create_label(
            this->model.footer,
            15.0f,
            brls::Application::getTheme()["brls/text_disabled"],
            true);
        addView(footer_label);

        registerClickAction([this](brls::View*) {
            if (this->model.action) {
                this->model.action();
            }
            return true;
        });

        apply_style(false);
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        apply_style(true);
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        apply_style(false);
    }

private:
    void apply_style(bool is_focused) {
        auto theme = brls::Application::getTheme();

        if (is_focused) {
            setBackgroundColor(nvgRGBAf(
                std::min(0.12f + this->model.accent_color.r * 0.42f, 1.0f),
                std::min(0.14f + this->model.accent_color.g * 0.42f, 1.0f),
                std::min(0.18f + this->model.accent_color.b * 0.42f, 1.0f),
                0.96f));
            setBorderColor(with_alpha(this->model.accent_color, 0.95f));
            setBorderThickness(3.0f);
            eyebrow_label->setTextColor(this->model.accent_color);
            title_label->setTextColor(nvgRGB(255, 255, 255));
            subtitle_label->setTextColor(nvgRGBA(245, 247, 250, 230));
            footer_label->setTextColor(with_alpha(this->model.accent_color, 0.95f));
        } else {
            setBackgroundColor(nvgRGBA(22, 27, 34, 235));
            setBorderColor(nvgRGBA(255, 255, 255, 28));
            setBorderThickness(1.0f);
            eyebrow_label->setTextColor(with_alpha(this->model.accent_color, 0.90f));
            title_label->setTextColor(theme["brls/text"]);
            subtitle_label->setTextColor(theme["brls/header/subtitle"]);
            footer_label->setTextColor(theme["brls/text_disabled"]);
        }
    }

    HomeCardModel model;
    brls::Label* eyebrow_label = nullptr;
    brls::Label* title_label = nullptr;
    brls::Label* subtitle_label = nullptr;
    brls::Label* footer_label = nullptr;
};

std::string build_error_dialog_message(const std::string& summary, const std::string& detail) {
    if (detail.empty()) {
        return summary;
    }

    return summary + "\n\n" + detail;
}

std::string format_bytes_brief(std::uint64_t size) {
    char buffer[32];
    if (size >= 1024ull * 1024ull) {
        const double value = static_cast<double>(size) / (1024.0 * 1024.0);
        std::snprintf(buffer, sizeof(buffer), "%.2f MB", value);
        return buffer;
    }

    if (size >= 1024ull) {
        const double value = static_cast<double>(size) / 1024.0;
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", value);
        return buffer;
    }

    std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(size));
    return buffer;
}

std::string build_iptv_loading_stage_text(switchbox::core::IptvPlaylistLoadStage stage) {
    switch (stage) {
        case switchbox::core::IptvPlaylistLoadStage::Starting:
            return tr("iptv_loading/stages/starting");
        case switchbox::core::IptvPlaylistLoadStage::OpeningConnection:
            return tr("iptv_loading/stages/opening_connection");
        case switchbox::core::IptvPlaylistLoadStage::DownloadingPlaylist:
            return tr("iptv_loading/stages/downloading_playlist");
        case switchbox::core::IptvPlaylistLoadStage::ParsingPlaylist:
            return tr("iptv_loading/stages/parsing_playlist");
        case switchbox::core::IptvPlaylistLoadStage::Finalizing:
            return tr("iptv_loading/stages/finalizing");
        default:
            return tr("iptv_loading/stages/starting");
    }
}

std::string build_iptv_loading_detail_text(
    const switchbox::core::IptvPlaylistLoadProgress& progress) {
    std::string text = build_iptv_loading_stage_text(progress.stage);
    if (progress.stage == switchbox::core::IptvPlaylistLoadStage::DownloadingPlaylist &&
        progress.bytes_read > 0) {
        text += " ";
        text += format_bytes_brief(progress.bytes_read);
        if (progress.total_bytes > 0) {
            text += " / ";
            text += format_bytes_brief(progress.total_bytes);
        }
    }
    return text;
}

std::string build_iptv_loading_percent_text(float progress) {
    const int percent = std::clamp(
        static_cast<int>(progress * 100.0f + 0.5f),
        0,
        100);
    return std::to_string(percent) + "%";
}

std::string build_iptv_playlist_error_message(const switchbox::core::IptvPlaylistResult& result) {
    if (!result.backend_available) {
        return tr("iptv_browser/backend_unavailable");
    }

    if (!result.success) {
        return build_error_dialog_message(
            tr("iptv_browser/open_failed"),
            result.error_message);
    }

    if (result.entries.empty()) {
        return build_error_dialog_message(
            tr("iptv_browser/open_failed"),
            tr("iptv_browser/empty"));
    }

    return tr("iptv_browser/open_failed");
}

struct IptvLoadingOverlayBuild {
    brls::View* view = nullptr;
    brls::Label* source_label = nullptr;
    brls::Label* detail_label = nullptr;
    brls::Label* percent_label = nullptr;
    brls::Box* fill_view = nullptr;
};

IptvLoadingOverlayBuild create_iptv_loading_overlay_content(
    const switchbox::core::IptvSourceSettings& source) {
    constexpr float kTrackWidth = 460.0f;
    constexpr float kTrackHeight = 22.0f;
    constexpr float kPercentWidth = 68.0f;
    constexpr float kTrackGap = 14.0f;
    auto theme = brls::Application::getTheme();

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setBackgroundColor(nvgRGBA(0, 0, 0, 112));
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setPadding(0);

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(720);
    panel->setPadding(30, 34, 28, 34);
    panel->setBackgroundColor(nvgRGBA(20, 25, 32, 244));
    panel->setCornerRadius(26.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);
    root->addView(panel);

    auto* title = create_label(tr("iptv_loading/title"), 24.0f, nvgRGBA(255, 255, 255, 245), false);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMargins(0, 0, 8, 0);
    panel->addView(title);

    auto* source_label = create_label(
        visible_iptv_title(source),
        16.0f,
        nvgRGBA(134, 220, 245, 230),
        true);
    source_label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    source_label->setMargins(0, 0, 18, 0);
    panel->addView(source_label);

    auto* detail_label = create_label(
        build_iptv_loading_stage_text(switchbox::core::IptvPlaylistLoadStage::Starting),
        17.0f,
        theme["brls/text_disabled"],
        false);
    detail_label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    detail_label->setMargins(0, 0, 16, 0);
    panel->addView(detail_label);

    auto* progress_row = new brls::Box(brls::Axis::ROW);
    progress_row->setAlignItems(brls::AlignItems::CENTER);
    progress_row->setAlignSelf(brls::AlignSelf::CENTER);
    progress_row->setWidth(kTrackWidth + kTrackGap + kPercentWidth);

    auto* track = new brls::Box(brls::Axis::ROW);
    track->setWidth(kTrackWidth);
    track->setHeight(kTrackHeight);
    track->setBackgroundColor(nvgRGBA(255, 255, 255, 36));
    track->setCornerRadius(kTrackHeight / 2.0f);
    track->setMargins(0, kTrackGap, 0, 0);

    auto* fill_view = new brls::Box(brls::Axis::ROW);
    fill_view->setWidth(0);
    fill_view->setHeight(kTrackHeight);
    fill_view->setBackgroundColor(nvgRGBA(104, 225, 124, 255));
    fill_view->setCornerRadius(kTrackHeight / 2.0f);
    track->addView(fill_view);
    progress_row->addView(track);

    auto* percent_label = create_label("0%", 18.0f, nvgRGBA(255, 255, 255, 235), true);
    percent_label->setWidth(kPercentWidth);
    percent_label->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    progress_row->addView(percent_label);

    panel->addView(progress_row);

    return {
        .view = root,
        .source_label = source_label,
        .detail_label = detail_label,
        .percent_label = percent_label,
        .fill_view = fill_view,
    };
}

struct HomeFinishedLoad {
    bool ready = false;
    bool user_cancelled = false;
    switchbox::core::IptvPlaylistResult result;
};

void apply_native_status_layout(brls::AppletFrame* frame) {
    frame->setTitle(tr("brand/app_name"));
}

}  // namespace

struct HomeActivity::IptvLoadState {
    std::mutex mutex;
    std::shared_ptr<std::atomic_bool> cancel_flag = std::make_shared<std::atomic_bool>(false);
    switchbox::core::IptvPlaylistLoadProgress progress;
    switchbox::core::IptvPlaylistResult result;
    bool finished = false;
    bool completion_consumed = false;
    bool user_cancelled = false;
};

HomeActivity::HomeActivity(const StartupContext& context)
    : brls::Activity() {
    (void)context;
}

HomeActivity::~HomeActivity() {
    this->iptv_loading_timer.stop();
    if (this->iptv_load_state != nullptr) {
        this->iptv_load_state->cancel_flag->store(true);
    }
}

brls::View* HomeActivity::createContentView() {
    return build_content();
}

void HomeActivity::onContentAvailable() {
    brls::Activity::onContentAvailable();
    if (this->cancel_loading_action_id != ACTION_NONE) {
        unregisterAction(this->cancel_loading_action_id);
        this->cancel_loading_action_id = ACTION_NONE;
    }

    this->cancel_loading_action_id = registerAction(
        brls::getStr("hints/back"),
        brls::BUTTON_B,
        [this](brls::View*) {
            if (!this->iptv_loading_visible) {
                return false;
            }

            append_home_iptv_debug_log("loading_overlay cancel_requested");
            cancel_iptv_loading(true);
            return true;
        },
        true,
        false,
        brls::SOUND_BACK);
}

brls::View* HomeActivity::build_content() {
    auto theme = brls::Application::getTheme();
    std::vector<HomeCardModel> cards;
    cards.push_back({
        .eyebrow = tr("home/cards/history/eyebrow"),
        .title = tr("home/cards/history/title"),
        .subtitle = tr("home/cards/history/subtitle"),
        .footer = tr("home/cards/history/detail"),
        .accent_color = nvgRGB(235, 164, 72),
        .action =
            []() {
                brls::Application::pushActivity(new PlaceholderActivity(make_history_section()));
            },
    });

    const auto& config = switchbox::core::AppConfigStore::current();
    for (const auto& source : config.iptv_sources) {
        if (!source.enabled) {
            continue;
        }

        cards.push_back({
            .eyebrow = tr("home/cards/iptv/eyebrow"),
            .title = visible_iptv_title(source),
            .subtitle = summarize_iptv_source(source),
            .footer = tr("home/cards/iptv/detail"),
            .accent_color = nvgRGB(73, 188, 218),
            .action =
                [this, source]() {
                    start_iptv_loading(source);
                },
        });
    }

    for (const auto& source : config.smb_sources) {
        if (!source.enabled) {
            continue;
        }

        cards.push_back({
            .eyebrow = tr("home/cards/smb/eyebrow"),
            .title = visible_smb_title(source),
            .subtitle = summarize_smb_source(source),
            .footer = tr("home/cards/smb/detail"),
            .accent_color = nvgRGB(94, 204, 151),
            .action =
                [source]() {
                    brls::Application::pushActivity(new SmbBrowserActivity(source));
                },
        });
    }

    const bool has_source_cards = cards.size() > 1;
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(14, 0, 28, 0);
    content->setJustifyContent(brls::JustifyContent::CENTER);
    content->setBackgroundColor(theme["brls/background"]);
    content->setPadding(10, 0, 24, 0);

    auto* cards_row = new brls::Box(brls::Axis::ROW);
    cards_row->setPadding(42, 44, 42, 44);
    cards_row->setAlignItems(brls::AlignItems::CENTER);

    for (const auto& card : cards) {
        cards_row->addView(new HomeSourceCard(card));
    }

    auto* carousel = new brls::HScrollingFrame();
    carousel->setGrow(1.0f);
    carousel->setScrollingIndicatorVisible(false);
    carousel->setContentView(cards_row);
    content->addView(carousel);

    if (!has_source_cards) {
        auto* empty_hint = create_label(
            tr("home/no_sources_hint"),
            17.0f,
            theme["brls/header/subtitle"]);
        empty_hint->setMargins(0, 56, 0, 0);
        content->addView(empty_hint);
    }

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setBackgroundColor(theme["brls/background"]);

    auto* frame = new brls::AppletFrame(content);
    frame->setGrow(1.0f);
    frame->registerAction(
        tr("actions/settings"),
        brls::BUTTON_START,
        [](brls::View*) {
            brls::Application::pushActivity(new SettingsActivity());
            return true;
        },
        false,
        false,
        brls::SOUND_CLICK);
    apply_native_status_layout(frame);
    root->addView(frame);

    auto overlay_build = create_iptv_loading_overlay_content({});
    this->iptv_loading_overlay = dynamic_cast<brls::Box*>(overlay_build.view);
    this->iptv_loading_source_label = overlay_build.source_label;
    this->iptv_loading_detail_label = overlay_build.detail_label;
    this->iptv_loading_percent_label = overlay_build.percent_label;
    this->iptv_loading_fill_view = overlay_build.fill_view;
    if (this->iptv_loading_overlay != nullptr) {
        this->iptv_loading_overlay->setPositionType(brls::PositionType::ABSOLUTE);
        this->iptv_loading_overlay->setPositionTop(0);
        this->iptv_loading_overlay->setPositionRight(0);
        this->iptv_loading_overlay->setPositionBottom(0);
        this->iptv_loading_overlay->setPositionLeft(0);
        this->iptv_loading_overlay->setVisibility(brls::Visibility::GONE);
        this->iptv_loading_overlay->setFocusable(true);
        this->iptv_loading_overlay->setHideHighlight(true);
        root->addView(this->iptv_loading_overlay);
    }

    return root;
}

void HomeActivity::start_iptv_loading(const switchbox::core::IptvSourceSettings& source) {
    if (this->iptv_loading_visible) {
        return;
    }

    this->pending_iptv_source = source;
    this->home_focus_before_iptv_loading = brls::Application::getCurrentFocus();
    this->iptv_load_state = std::make_shared<IptvLoadState>();
    this->iptv_loading_completion_handled = false;
    this->iptv_loading_visible = true;

    if (this->iptv_loading_source_label != nullptr) {
        this->iptv_loading_source_label->setText(visible_iptv_title(source));
    }

    append_home_iptv_debug_log(
        "loading_overlay start source=" + sanitize_home_log_text(visible_iptv_title(source)));
    refresh_iptv_loading_overlay();
    if (this->iptv_loading_overlay != nullptr) {
        this->iptv_loading_overlay->setVisibility(brls::Visibility::VISIBLE);
        brls::Application::giveFocus(this->iptv_loading_overlay);
    }

    this->iptv_loading_timer.stop();
    this->iptv_loading_timer.setPeriod(90);
    this->iptv_loading_timer.setCallback([this]() {
        refresh_iptv_loading_overlay();
        handle_iptv_loading_completion();
    });
    this->iptv_loading_timer.start();

    auto state = this->iptv_load_state;
    const auto source_copy = source;
    brls::async([state, source_copy]() {
        auto result = switchbox::core::load_iptv_playlist(
            source_copy,
            state->cancel_flag,
            [state](const switchbox::core::IptvPlaylistLoadProgress& progress) {
                std::scoped_lock lock(state->mutex);
                if (state->user_cancelled) {
                    return;
                }
                state->progress = progress;
            });

        brls::sync([state, result = std::move(result)]() mutable {
            std::scoped_lock lock(state->mutex);
            if (!state->user_cancelled) {
                state->progress.progress = 1.0f;
                state->progress.stage = switchbox::core::IptvPlaylistLoadStage::Finalizing;
            }
            state->result = std::move(result);
            state->finished = true;
        });
    });
}

void HomeActivity::cancel_iptv_loading(bool user_cancelled) {
    auto state = this->iptv_load_state;
    if (state != nullptr) {
        std::scoped_lock lock(state->mutex);
        if (user_cancelled) {
            state->user_cancelled = true;
        }
        state->cancel_flag->store(true);
    }

    hide_iptv_loading_overlay(true);
    this->iptv_load_state.reset();
}

void HomeActivity::refresh_iptv_loading_overlay() {
    if (!this->iptv_loading_visible || this->iptv_load_state == nullptr) {
        return;
    }

    switchbox::core::IptvPlaylistLoadProgress progress;
    {
        std::scoped_lock lock(this->iptv_load_state->mutex);
        progress = this->iptv_load_state->progress;
    }

    if (this->iptv_loading_detail_label != nullptr) {
        this->iptv_loading_detail_label->setText(build_iptv_loading_detail_text(progress));
    }
    if (this->iptv_loading_percent_label != nullptr) {
        this->iptv_loading_percent_label->setText(build_iptv_loading_percent_text(progress.progress));
    }
    if (this->iptv_loading_fill_view != nullptr) {
        constexpr float kTrackWidth = 460.0f;
        const float progress_width = std::clamp(progress.progress, 0.0f, 1.0f) * kTrackWidth;
        this->iptv_loading_fill_view->setWidth(progress_width);
    }
}

void HomeActivity::handle_iptv_loading_completion() {
    if (this->iptv_loading_completion_handled || this->iptv_load_state == nullptr) {
        return;
    }

    HomeFinishedLoad finished;
    {
        std::scoped_lock lock(this->iptv_load_state->mutex);
        if (!this->iptv_load_state->finished || this->iptv_load_state->completion_consumed) {
            return;
        }

        this->iptv_load_state->completion_consumed = true;
        finished.ready = true;
        finished.user_cancelled = this->iptv_load_state->user_cancelled;
        finished.result = std::move(this->iptv_load_state->result);
    }

    if (!finished.ready) {
        return;
    }

    this->iptv_loading_completion_handled = true;
    this->iptv_loading_timer.stop();

    if (finished.user_cancelled) {
        append_home_iptv_debug_log("loading_overlay completion ignored because user cancelled");
        this->iptv_load_state.reset();
        return;
    }

    if (finished.result.backend_available &&
        finished.result.success &&
        !finished.result.entries.empty()) {
        auto source_copy = this->pending_iptv_source;
        append_home_iptv_debug_log(
            "loading_overlay success entries=" + std::to_string(finished.result.entries.size()) +
            " push_browser_from_home");
        hide_iptv_loading_overlay(false);
        this->iptv_load_state.reset();
        brls::Application::pushActivity(new IptvBrowserActivity(source_copy, std::move(finished.result)));
        return;
    }

    const std::string error_message = build_iptv_playlist_error_message(finished.result);
    append_home_iptv_debug_log("loading_overlay failed message=" + sanitize_home_log_text(error_message));
    hide_iptv_loading_overlay(true);
    this->iptv_load_state.reset();
    brls::delay(1, [error_message]() {
        auto* dialog = new brls::Dialog(error_message);
        dialog->addButton(brls::getStr("hints/ok"), []() {});
        dialog->open();
    });
}

void HomeActivity::hide_iptv_loading_overlay(bool restore_focus) {
    this->iptv_loading_timer.stop();
    this->iptv_loading_visible = false;
    this->iptv_loading_completion_handled = false;

    if (this->iptv_loading_overlay != nullptr) {
        this->iptv_loading_overlay->setVisibility(brls::Visibility::GONE);
    }

    if (restore_focus) {
        restore_home_focus();
    }
}

void HomeActivity::restore_home_focus() {
    if (this->home_focus_before_iptv_loading != nullptr &&
        this->home_focus_before_iptv_loading->getVisibility() == brls::Visibility::VISIBLE) {
        brls::Application::giveFocus(this->home_focus_before_iptv_loading);
        return;
    }

    if (auto* fallback = getDefaultFocus()) {
        brls::Application::giveFocus(fallback);
    }
}

}  // namespace switchbox::app
