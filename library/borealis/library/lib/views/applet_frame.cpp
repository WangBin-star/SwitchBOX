/*
    Copyright 2019-2021 natinusala
    Copyright 2021 XITRIX
    Copyright 2019 p-sam

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/core/util.hpp>
#include <borealis/views/applet_frame.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/hint.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace brls::literals;

namespace brls
{

const std::string appletFrameXML = R"xml(
    <brls:Box
        width="auto"
        height="auto"
        axis="column"
        justifyContent="spaceBetween">

        <!-- Header -->
        <brls:Box
            id="brls/applet_frame/header"
            width="auto"
            height="@style/brls/applet_frame/header_height"
            axis="row"
            justifyContent="spaceBetween"
            paddingLeft="@style/brls/applet_frame/header_padding_sides"
            paddingRight="@style/brls/applet_frame/header_padding_sides"
            marginLeft="@style/brls/applet_frame/padding_sides"
            marginRight="@style/brls/applet_frame/padding_sides"
            lineColor="@theme/brls/applet_frame/separator"
            lineBottom="1px">

            <brls:Box
                width="auto"
                height="auto"
                axis="row"
                paddingTop="@style/brls/applet_frame/header_padding_top_bottom"
                paddingBottom="@style/brls/applet_frame/header_padding_top_bottom">
                <brls:Image
                    id="brls/applet_frame/title_icon"
                    width="auto"
                    height="auto"
                    marginRight="@style/brls/applet_frame/header_image_title_spacing"
                    visibility="gone" />

                <brls:Label
                    id="brls/applet_frame/title_label"
                    width="auto"
                    height="auto"
                    marginTop="@style/brls/applet_frame/header_title_top_offset"
                    fontSize="@style/brls/applet_frame/header_title_font_size" />
            </brls:Box>

            <brls:Box
                width="auto"
                height="auto"
                marginTop="@style/brls/applet_frame/header_title_top_offset"
                axis="row"
                alignItems="center">

                <brls:Box
                    id="brls/applet_frame/hint_box"
                    width="auto"
                    height="auto"
                    axis="row"
                    alignItems="center"/>

                <brls:Box
                    id="brls/applet_frame/status_box"
                    width="auto"
                    height="auto"
                    axis="row"
                    alignItems="center"
                    marginLeft="16">

                    <brls:Label
                        id="brls/hints/time"
                        width="auto"
                        height="auto"
                        verticalAlign="center"
                        fontSize="21.5"
                        marginRight="21" />

                    <brls:Wireless
                        id="brls/wireless"
                        marginRight="21"
                        marginBottom="5"/>

                    <brls:Battery
                        id="brls/battery"
                        marginBottom="5"/>

                </brls:Box>

            </brls:Box>

        </brls:Box>

        <!-- Content will be injected here with grow="1.0" -->

        <!--
            Footer
            Direction inverted so that the bottom left text can be
            set to visibility="gone" without affecting the hint
        -->
        <brls:BottomBar
            id="brls/applet_frame/footer"/>

    </brls:Box>
)xml";

AppletFrame::AppletFrame()
{
    this->inflateFromXMLString(appletFrameXML);

    this->forwardXMLAttribute("iconInterpolation", this->icon, "interpolation");

    BRLS_REGISTER_ENUM_XML_ATTRIBUTE(
        "style", HeaderStyle, this->setHeaderStyle,
        {
            { "regular", HeaderStyle::REGULAR },
            { "popup", HeaderStyle::POPUP },
        });

    this->registerBoolXMLAttribute("headerHidden", [this](bool value)
        { this->setHeaderVisibility(value ? Visibility::GONE : Visibility::VISIBLE); });

    this->registerBoolXMLAttribute("footerHidden", [this](bool value)
        {
        if(HIDE_BOTTOM_BAR)
            this->setFooterVisibility(Visibility::GONE);
        else
            this->setFooterVisibility(value ? Visibility::GONE : Visibility::VISIBLE); });

    this->registerAction(
        "hints/back"_i18n, BUTTON_B, [this](View* view)
        {
            this->contentViewStack.back()->dismiss();
            return true; },
        false, false, SOUND_BACK);

#ifdef __SWITCH__
    Platform* platform = Application::getPlatform();
    statusBox->setVisibility(Visibility::VISIBLE);
    battery->setVisibility(platform->canShowBatteryLevel() ? Visibility::VISIBLE : Visibility::GONE);
    wireless->setVisibility(platform->canShowWirelessLevel() ? Visibility::VISIBLE : Visibility::GONE);
#else
    statusBox->setVisibility(Visibility::GONE);
#endif
}

AppletFrame::AppletFrame(View* contentView)
    : AppletFrame::AppletFrame()
{
    contentViewStack.push_back(contentView);
    setContentView(contentView);
}

void AppletFrame::setIcon(std::string path)
{
    if (path.empty())
    {
        this->icon->setVisibility(Visibility::GONE);
    }
    else
    {
        this->icon->setVisibility(Visibility::VISIBLE);
        this->icon->setImageFromFile(path);
    }
}

void AppletFrame::setHeaderVisibility(Visibility visibility)
{
    header->setVisibility(visibility);
}

void AppletFrame::setFooterVisibility(Visibility visibility)
{
    footer->setVisibility(visibility);
}

void AppletFrame::setTitle(std::string title)
{
    this->title->setText(title);
}

void AppletFrame::updateStatusText()
{
#ifdef __SWITCH__
    auto timeNow   = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(timeNow);
    auto tm        = *std::localtime(&in_time_t);

    std::stringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    if (ss.str() != statusTimeText)
    {
        statusTimeText = ss.str();
        time->setText(statusTimeText);
    }
#endif
}

void AppletFrame::pushContentView(View* view)
{
    contentViewStack.push_back(view);
    setContentView(view);
    Application::giveFocus(view);
}

void AppletFrame::popContentView(std::function<void(void)> cb)
{
    if (contentViewStack.size() <= 1)
    {
        if (!Application::popActivity(TransitionAnimation::FADE, cb))
        {
#ifndef IOS // Do not close the app in iOS
            auto dialog = new brls::Dialog("hints/exit_hint"_i18n);
            dialog->addButton("hints/cancel"_i18n, []() {});
            dialog->addButton("hints/ok"_i18n, []()
                { Application::quit(); });
            dialog->open();
#endif
        }
        return;
    }

    View* lastView = contentViewStack.back();
    contentViewStack.pop_back();

    View* newView = contentViewStack.back();
    setContentView(newView);
    Application::giveFocus(newView);
    cb();

    lastView->freeView();
}

void AppletFrame::setContentView(View* view)
{
    if (this->contentView)
    {
        // Remove the node
        this->removeView(this->contentView, false);
        this->contentView = nullptr;
    }

    if (!view)
        return;

    this->contentView = view;

    this->contentView->setDimensions(View::AUTO, View::AUTO);
    this->contentView->setGrow(1.0f);

    this->addView(this->contentView, 1);

    this->updateAppletFrameItem();
}

void AppletFrame::draw(NVGcontext* vg, float x, float y, float width, float height, Style style, FrameContext* ctx)
{
    updateStatusText();
    Box::draw(vg, x, y, width, height, style, ctx);
}

void AppletFrame::handleXMLElement(tinyxml2::XMLElement* element)
{
    if (this->contentView)
        fatal("brls:AppletFrame can only have one child XML element");

    View* view = View::createFromXMLElement(element);
    contentViewStack.push_back(view);
    setContentView(view);
}

void AppletFrame::updateAppletFrameItem()
{
    hintBox->clearViews(false);

    setTitle(contentView->getAppletFrameItem()->title);
    setIcon(contentView->getAppletFrameItem()->iconPath);

    if (contentView->getAppletFrameItem()->getHintView())
        hintBox->addView(contentView->getAppletFrameItem()->getHintView());
}

void AppletFrame::setHeaderStyle(HeaderStyle style)
{
    this->style = style;

    Style appStyle = Application::getStyle();
    switch (style)
    {
        case HeaderStyle::REGULAR:
            header->setHeight(appStyle["brls/applet_frame/header_height"]);
            title->setFontSize(appStyle["brls/applet_frame/header_title_font_size"]);
            break;
        case HeaderStyle::POPUP:
            //            header->setHeight(appStyle["brls/applet_frame/dropdown_header_height"]);
            //            title->setFontSize(appStyle["brls/applet_frame/dropdown_header_title_font_size"]);
            break;
        default:
            break;
    }
}

View* AppletFrame::create()
{
    return new AppletFrame();
}

} // namespace brls
