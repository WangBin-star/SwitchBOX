#include "switchbox/app/home_activity.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <borealis/core/i18n.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/h_scrolling_frame.hpp>
#include <borealis/views/label.hpp>

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

PlaceholderSection make_iptv_section(const switchbox::core::IptvSourceSettings& source) {
    return {
        .title = visible_iptv_title(source),
        .subtitle = summarize_iptv_source(source),
        .checkpoints =
            {
                tr("sections/iptv/checkpoints/1"),
                tr("sections/iptv/checkpoints/2"),
                tr("sections/iptv/checkpoints/3"),
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

void apply_native_status_layout(brls::AppletFrame* frame) {
    frame->setTitle(tr("brand/app_name"));
}

std::vector<HomeCardModel> build_home_cards() {
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
                [source]() {
                    brls::Application::pushActivity(new PlaceholderActivity(make_iptv_section(source)));
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

    return cards;
}

brls::View* create_home_content() {
    auto theme = brls::Application::getTheme();
    const std::vector<HomeCardModel> cards = build_home_cards();
    const bool has_source_cards = cards.size() > 1;
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(14, 0, 28, 0);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setBackgroundColor(theme["brls/background"]);
    root->setPadding(10, 0, 24, 0);

    auto* cards_row = new brls::Box(brls::Axis::ROW);
    cards_row->setPadding(54, 56, 54, 56);
    cards_row->setAlignItems(brls::AlignItems::CENTER);
    cards_row->setPadding(42, 44, 42, 44);

    for (const auto& card : cards) {
        cards_row->addView(new HomeSourceCard(card));
    }

    auto* carousel = new brls::HScrollingFrame();
    carousel->setGrow(1.0f);
    carousel->setScrollingIndicatorVisible(false);
    carousel->setContentView(cards_row);
    root->addView(carousel);

    if (!has_source_cards) {
        auto* empty_hint = create_label(
            tr("home/no_sources_hint"),
            17.0f,
            theme["brls/header/subtitle"]);
        empty_hint->setMargins(0, 56, 0, 0);
        root->addView(empty_hint);
    }

    auto* frame = new brls::AppletFrame(root);
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
    return frame;
}

}  // namespace

HomeActivity::HomeActivity(const StartupContext& context)
    : brls::Activity(create_home_content()) {
    (void)context;
}

}  // namespace switchbox::app
