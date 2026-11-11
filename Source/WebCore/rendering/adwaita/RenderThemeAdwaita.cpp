/*
 * Copyright (C) 2014, 2020 Igalia S.L.
 * Copyright (C) 2025 Samuel Weinig <sam@webkit.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RenderThemeAdwaita.h"

#if USE(THEME_ADWAITA)

#include "Adwaita.h"
#include "Color.h"
#include "Document.h"
#include "FloatRoundedRect.h"
#include "GraphicsContext.h"
#include "HTMLInputElement.h"
#include "HTMLMediaElement.h"
#include "MediaControlTextTrackContainerElement.h"
#include "Page.h"
#include "PaintInfo.h"
#include "RenderBox.h"
#include "RenderObjectInlines.h"
#include "RenderProgress.h"
#include "StyleComputedStyle+GettersInlines.h"
#include "StyleComputedStyle+InitialInlines.h"
#include "StyleComputedStyle+SettersInlines.h"
#include "StyleLengthResolution.h"
#include "StylePadding.h"
#include "StylePrimitiveNumericTypes+Evaluation.h"
#include "ThemeAdwaita.h"
#include "TimeRanges.h"
#include "UserAgentScripts.h"
#include "UserAgentStyleSheets.h"
#include <wtf/text/Base64.h>

#if PLATFORM(WIN)
#include "WebCoreBundleWin.h"
#include <wtf/FileSystem.h>
#endif

#if PLATFORM(GTK) || PLATFORM(WPE)
#include "SystemSettings.h"
#endif

#if USE(GLIB)
#include <wtf/glib/GSpanExtras.h>
#endif

namespace WebCore {
using namespace CSS::Literals;
using namespace WebCore::Adwaita;

RenderThemeAdwaita& RenderTheme::singleton()
{
    static MainThreadNeverDestroyed<RenderThemeAdwaita> theme;
    return theme;
}

RenderThemeAdwaita::~RenderThemeAdwaita() = default;

bool RenderThemeAdwaita::canCreateControlPartForRenderer(const RenderElement& renderer) const
{
    switch (renderer.style().usedAppearance()) {
    case StyleAppearance::Button:
    case StyleAppearance::Checkbox:
    case StyleAppearance::ColorWell:
    case StyleAppearance::DefaultButton:
    case StyleAppearance::InnerSpinButton:
    case StyleAppearance::Menulist:
    case StyleAppearance::ProgressBar:
    case StyleAppearance::PushButton:
    case StyleAppearance::Radio:
    case StyleAppearance::SearchField:
    case StyleAppearance::SliderThumbHorizontal:
    case StyleAppearance::SliderThumbVertical:
    case StyleAppearance::SliderHorizontal:
    case StyleAppearance::SliderVertical:
    case StyleAppearance::SquareButton:
        return true;
    default:
        break;
    }
    return false;
}

bool RenderThemeAdwaita::canCreateControlPartForBorderOnly(const RenderElement& renderer) const
{
    switch (renderer.style().usedAppearance()) {
    case StyleAppearance::Listbox:
    case StyleAppearance::TextArea:
    case StyleAppearance::TextField:
        return true;
    default:
        break;
    }
    return false;
}

bool RenderThemeAdwaita::canCreateControlPartForDecorations(const RenderElement& renderer) const
{
    return renderer.style().usedAppearance() == StyleAppearance::MenulistButton;
}

bool RenderThemeAdwaita::supportsFocusRing(const RenderElement&, const Style::ComputedStyle& style) const
{
    switch (style.usedAppearance()) {
    case StyleAppearance::PushButton:
    case StyleAppearance::Button:
    case StyleAppearance::TextField:
    case StyleAppearance::TextArea:
    case StyleAppearance::SearchField:
    case StyleAppearance::Menulist:
    case StyleAppearance::Radio:
    case StyleAppearance::Checkbox:
    case StyleAppearance::SliderHorizontal:
    case StyleAppearance::SliderVertical:
        return true;
    default:
        break;
    }

    return false;
}

bool RenderThemeAdwaita::shouldHaveCapsLockIndicator(const HTMLInputElement& element) const
{
    return element.isPasswordField();
}

Color RenderThemeAdwaita::platformActiveSelectionBackgroundColor(OptionSet<StyleColorOptions>) const
{
    return systemAccentColor().colorWithAlphaMultipliedBy(0.3);
}

Color RenderThemeAdwaita::platformInactiveSelectionBackgroundColor(OptionSet<StyleColorOptions> options) const
{
    if (options.contains(StyleColorOptions::UseDarkAppearance))
        return SRGBA<uint8_t> { 255, 255, 255, 25 };

    return SRGBA<uint8_t> { 0, 0, 0, 25 };
}

Color RenderThemeAdwaita::platformActiveSelectionForegroundColor(OptionSet<StyleColorOptions>) const
{
    return { };
}

Color RenderThemeAdwaita::platformInactiveSelectionForegroundColor(OptionSet<StyleColorOptions>) const
{
    return { };
}

Color RenderThemeAdwaita::platformActiveListBoxSelectionBackgroundColor(OptionSet<StyleColorOptions> options) const
{
    return platformActiveListBoxSelectionForegroundColor(options).colorWithAlpha(0.15);
}

Color RenderThemeAdwaita::platformInactiveListBoxSelectionBackgroundColor(OptionSet<StyleColorOptions> options) const
{
    return platformInactiveListBoxSelectionForegroundColor(options).colorWithAlpha(0.15);
}

Color RenderThemeAdwaita::platformActiveListBoxSelectionForegroundColor(OptionSet<StyleColorOptions> options) const
{
    return options.contains(StyleColorOptions::UseDarkAppearance) ? Color::white : Color::black;
}

Color RenderThemeAdwaita::platformInactiveListBoxSelectionForegroundColor(OptionSet<StyleColorOptions> options) const
{
    return platformActiveListBoxSelectionForegroundColor(options);
}

Color RenderThemeAdwaita::platformFocusRingColor(OptionSet<StyleColorOptions>) const
{
    return systemFocusRingColor();
}

float RenderThemeAdwaita::platformFocusRingWidth() const
{
    return Adwaita::focusLineWidth;
}

void RenderThemeAdwaita::platformColorsDidChange()
{
    static_cast<ThemeAdwaita&>(Theme::singleton()).platformColorsDidChange();
    RenderTheme::platformColorsDidChange();
}

String RenderThemeAdwaita::extraDefaultStyleSheet()
{
    return StringImpl::createWithoutCopying(themeAdwaitaUserAgentStyleSheet);
}

#if ENABLE(VIDEO)

Vector<String, 2> RenderThemeAdwaita::mediaControlsScripts()
{
    return { StringImpl::createWithoutCopying(ModernMediaControlsJavaScript) };
}

Vector<String, 2> RenderThemeAdwaita::mediaControlsStyleSheets(const HTMLMediaElement&)
{
    if (m_mediaControlsStyleSheet.isEmpty())
        m_mediaControlsStyleSheet = StringImpl::createWithoutCopying(ModernMediaControlsUserAgentStyleSheet);
    return { m_mediaControlsStyleSheet };
}

RefPtr<FragmentedSharedBuffer> RenderThemeAdwaita::mediaControlsImageDataForIconNameAndType(const String& iconName, const String& iconType)
{
#if USE(GLIB)
    auto path = makeString("/org/webkit/media-controls/"_s, iconName, '.', iconType);
    GRefPtr data = adoptGRef(g_resources_lookup_data(path.utf8().legacyCStringPointer(), G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr));
    if (!data)
        return nullptr;
    return SharedBuffer::create(span(data));
#elif PLATFORM(WIN)
    auto path = webKitBundlePath(iconName, iconType, "media-controls"_s);
    auto data = FileSystem::readEntireFile(path);
    if (!data)
        return nullptr;
    return SharedBuffer::create(WTF::move(*data));
#else
    return nullptr;
#endif
}

String RenderThemeAdwaita::mediaControlsBase64StringForIconNameAndType(const String& iconName, const String& iconType)
{
#if USE(GLIB)
    auto path = makeString("/org/webkit/media-controls/"_s, iconName, '.', iconType);
    GRefPtr data = adoptGRef(g_resources_lookup_data(path.utf8().legacyCStringPointer(), G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr));
    if (!data)
        return emptyString();
    return base64EncodeToString(span(data));
#elif PLATFORM(WIN)
    auto path = webKitBundlePath(iconName, iconType, "media-controls"_s);
    auto data = FileSystem::readEntireFile(path);
    if (!data)
        return { };
    return base64EncodeToString(data->span());
#else
    return { };
#endif
}

String RenderThemeAdwaita::mediaControlsFormattedStringForDuration(double durationInSeconds)
{
    // FIXME: Format this somehow, maybe through GDateTime?
    return makeString(durationInSeconds);
}
#endif // ENABLE(VIDEO)

Color RenderThemeAdwaita::systemColor(CSSValueID cssValueID, OptionSet<StyleColorOptions> options) const
{
    const bool useDarkAppearance = options.contains(StyleColorOptions::UseDarkAppearance);

#if OS(MACOS)
    // The system colours of CSS as Safari answers them on a Mac, light and dark. Pages read them
    // back, so they are part of what a Mac looks like to a page.
    auto macColor = [&](SRGBA<uint8_t> light, SRGBA<uint8_t> dark) -> Color {
        return { useDarkAppearance ? dark : light, Color::Flags::Semantic };
    };
    switch (cssValueID) {
    case CSSValueID::Buttonface:
    case CSSValueID::Threedface:
        return macColor(SRGBA<uint8_t> { 192, 192, 192 }, SRGBA<uint8_t> { 255, 255, 255, 63 });
    case CSSValueID::Activebuttontext:
    case CSSValueID::Buttontext:
    case CSSValueID::Fieldtext:
    case CSSValueID::Activecaption:
    case CSSValueID::Menutext:
    case CSSValueID::Windowtext:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 216 }, SRGBA<uint8_t> { 255, 255, 255, 216 });
    case CSSValueID::Field:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 255, 255, 255, 63 });
    case CSSValueID::Graytext:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 63 }, SRGBA<uint8_t> { 255, 255, 255, 63 });
    case CSSValueID::Highlight:
        return macColor(SRGBA<uint8_t> { 128, 188, 254, 153 }, SRGBA<uint8_t> { 63, 99, 139, 204 });
    case CSSValueID::Highlighttext:
    case CSSValueID::Inactivecaptiontext:
        return macColor(SRGBA<uint8_t> { 0, 0, 0 }, SRGBA<uint8_t> { 255, 255, 255 });
    case CSSValueID::Activeborder:
        return macColor(SRGBA<uint8_t> { 0, 103, 244 }, SRGBA<uint8_t> { 26, 169, 255 });
    case CSSValueID::Appworkspace:
        return macColor(SRGBA<uint8_t> { 170, 170, 170 }, SRGBA<uint8_t> { 255, 255, 255 });
    case CSSValueID::Buttonhighlight:
    case CSSValueID::Threedlightshadow:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 255, 255, 255, 25 });
    case CSSValueID::Buttonshadow:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 68 }, SRGBA<uint8_t> { 0, 0, 0, 68 });
    case CSSValueID::Inactiveborder:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 30, 30, 30 });
    case CSSValueID::Infobackground:
        return macColor(SRGBA<uint8_t> { 251, 252, 197 }, SRGBA<uint8_t> { 251, 252, 197 });
    case CSSValueID::Menu:
        return macColor(SRGBA<uint8_t> { 246, 246, 246 }, SRGBA<uint8_t> { 55, 55, 55 });
    case CSSValueID::Scrollbar:
    case CSSValueID::Windowframe:
        return macColor(SRGBA<uint8_t> { 170, 170, 170 }, SRGBA<uint8_t> { 170, 170, 170 });
    case CSSValueID::Threeddarkshadow:
    case CSSValueID::Threedshadow:
        return macColor(SRGBA<uint8_t> { 0, 0, 0 }, SRGBA<uint8_t> { 0, 0, 0 });
    case CSSValueID::Threedhighlight:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 180, 180, 180 });

    // The -apple-system-* colours, and 'text', measured from Safari on this Mac by setting each
    // keyword on a light- and a dark-schemed element and reading its computed colour back.
    case CSSValueID::AppleSystemHeaderText:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 216 }, SRGBA<uint8_t> { 255, 255, 255 });
    case CSSValueID::AppleSystemLabel:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 216 }, SRGBA<uint8_t> { 255, 255, 255, 216 });
    case CSSValueID::AppleSystemPlaceholderText:
    case CSSValueID::AppleSystemSecondaryLabel:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 127 }, SRGBA<uint8_t> { 255, 255, 255, 140 });
    case CSSValueID::AppleSystemTertiaryLabel:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 66 }, SRGBA<uint8_t> { 255, 255, 255, 63 });
    case CSSValueID::AppleSystemQuaternaryLabel:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 25 }, SRGBA<uint8_t> { 255, 255, 255, 25 });
    case CSSValueID::AppleSystemQuinaryLabel:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 12 }, SRGBA<uint8_t> { 255, 255, 255, 12 });
    case CSSValueID::AppleSystemGrid:
        return macColor(SRGBA<uint8_t> { 230, 230, 230 }, SRGBA<uint8_t> { 26, 26, 26 });
    case CSSValueID::AppleSystemSeparator:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 26 }, SRGBA<uint8_t> { 255, 255, 255, 31 });
    case CSSValueID::AppleSystemContainerBorder:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 63 }, SRGBA<uint8_t> { 255, 255, 255, 63 });
    case CSSValueID::AppleSystemTextBackground:
    case CSSValueID::AppleSystemControlBackground:
    case CSSValueID::AppleSystemBackground:
    case CSSValueID::AppleSystemSecondaryBackground:
    case CSSValueID::AppleSystemTertiaryBackground:
    case CSSValueID::AppleSystemGroupedBackground:
    case CSSValueID::AppleSystemSecondaryGroupedBackground:
    case CSSValueID::AppleSystemTertiaryGroupedBackground:
    case CSSValueID::WebkitControlBackground:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 30, 30, 30 });
    case CSSValueID::AppleSystemSelectedContentBackground:
        return macColor(SRGBA<uint8_t> { 0, 99, 225 }, SRGBA<uint8_t> { 0, 88, 208 });
    case CSSValueID::AppleSystemUnemphasizedSelectedContentBackground:
    case CSSValueID::AppleSystemUnemphasizedSelectedTextBackground:
        return macColor(SRGBA<uint8_t> { 220, 220, 220 }, SRGBA<uint8_t> { 70, 70, 70 });
    case CSSValueID::AppleSystemSelectedTextBackground:
        return macColor(SRGBA<uint8_t> { 128, 188, 254, 153 }, SRGBA<uint8_t> { 63, 99, 139, 204 });
    case CSSValueID::AppleSystemEvenAlternatingContentBackground:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 30, 30, 30 });
    case CSSValueID::AppleSystemOddAlternatingContentBackground:
        return macColor(SRGBA<uint8_t> { 244, 245, 245 }, SRGBA<uint8_t> { 255, 255, 255, 12 });
    case CSSValueID::AppleSystemAlternateSelectedText:
        return macColor(SRGBA<uint8_t> { 255, 255, 255 }, SRGBA<uint8_t> { 255, 255, 255 });
    case CSSValueID::AppleSystemSelectedText:
    case CSSValueID::AppleSystemUnemphasizedSelectedText:
    case CSSValueID::Text:
        return macColor(SRGBA<uint8_t> { 0, 0, 0 }, SRGBA<uint8_t> { 255, 255, 255 });
    case CSSValueID::AppleSystemFindHighlightBackground:
        return macColor(SRGBA<uint8_t> { 255, 255, 0 }, SRGBA<uint8_t> { 255, 255, 0 });
    case CSSValueID::AppleSystemAppHighlightBackground:
        return macColor(SRGBA<uint8_t> { 0, 0, 0, 0 }, SRGBA<uint8_t> { 0, 0, 0, 0 });
    case CSSValueID::AppleSystemControlAccent:
        return macColor(SRGBA<uint8_t> { 0, 122, 255 }, SRGBA<uint8_t> { 0, 122, 255 });
    case CSSValueID::AppleWirelessPlaybackTargetActive:
    case CSSValueID::AppleSystemBlue:
        return macColor(SRGBA<uint8_t> { 0, 136, 255 }, SRGBA<uint8_t> { 0, 145, 255 });
    case CSSValueID::AppleSystemBrown:
        return macColor(SRGBA<uint8_t> { 172, 127, 94 }, SRGBA<uint8_t> { 183, 138, 102 });
    case CSSValueID::AppleSystemGray:
        return macColor(SRGBA<uint8_t> { 142, 142, 147 }, SRGBA<uint8_t> { 152, 152, 157 });
    case CSSValueID::AppleSystemGreen:
        return macColor(SRGBA<uint8_t> { 52, 199, 89 }, SRGBA<uint8_t> { 48, 209, 88 });
    case CSSValueID::AppleSystemOrange:
        return macColor(SRGBA<uint8_t> { 255, 141, 40 }, SRGBA<uint8_t> { 255, 146, 48 });
    case CSSValueID::AppleSystemPink:
        return macColor(SRGBA<uint8_t> { 255, 45, 85 }, SRGBA<uint8_t> { 255, 55, 95 });
    case CSSValueID::AppleSystemPurple:
        return macColor(SRGBA<uint8_t> { 203, 48, 224 }, SRGBA<uint8_t> { 219, 52, 242 });
    case CSSValueID::AppleSystemRed:
        return macColor(SRGBA<uint8_t> { 255, 56, 60 }, SRGBA<uint8_t> { 255, 66, 69 });
    case CSSValueID::AppleSystemYellow:
        return macColor(SRGBA<uint8_t> { 255, 204, 0 }, SRGBA<uint8_t> { 255, 214, 0 });
    default:
        break;
    }
#endif

    switch (cssValueID) {
    case CSSValueID::Activebuttontext:
    case CSSValueID::Buttontext:
        if (useDarkAppearance)
            return { buttonTextColorDark, Color::Flags::Semantic };
        return { buttonTextColorLight, Color::Flags::Semantic };

    case CSSValueID::Graytext:
        if (useDarkAppearance)
            return { buttonTextDisabledColorDark, Color::Flags::Semantic };
        return { buttonTextDisabledColorLight, Color::Flags::Semantic };

    case CSSValueID::Canvas:
        if (useDarkAppearance)
            return { SRGBA<uint8_t> { 30, 30, 30 }, Color::Flags::Semantic };
        return { Color::white, Color::Flags::Semantic };

    case CSSValueID::Field:
#if PLATFORM(COCOA)
    case CSSValueID::WebkitControlBackground:
#endif
        if (useDarkAppearance)
            return { textFieldBackgroundColorDark, Color::Flags::Semantic };
        return { textFieldBackgroundColorLight, Color::Flags::Semantic };

    case CSSValueID::Canvastext:
    case CSSValueID::Fieldtext:
        if (useDarkAppearance)
            return { Color::white, Color::Flags::Semantic };
        return { Color::black, Color::Flags::Semantic };

    case CSSValueID::Highlight:
        // Hardcoded to avoid exposing a user appearance preference to the web for fingerprinting.
        return { SRGBA<uint8_t> { 52, 132, 228 }, Color::Flags::Semantic };

    case CSSValueID::Highlighttext:
        return { Color::white, Color::Flags::Semantic };

    default:
        return RenderTheme::systemColor(cssValueID, options);
    }
}

bool RenderThemeAdwaita::isControlStyled(const Style::ComputedStyle& style) const
{
    auto appearance = style.usedAppearance();
    if (appearance == StyleAppearance::TextField || appearance == StyleAppearance::TextArea || appearance == StyleAppearance::SearchField || appearance == StyleAppearance::Listbox)
        return style.nativeAppearanceDisabled();

    return RenderTheme::isControlStyled(style);
}

#if OS(MACOS)

// On a Mac the controls are drawn by this theme and measured like the Mac's own: the paddings, the
// minimum sizes and the rounding below are those of WebKit's theme for macOS, so that a control
// takes the room, and reports the computed style, that pages get from every other browser there.

static void applyPaddingIfNotExplicitlySet(Style::ComputedStyle& style, Style::PaddingBox paddingBox)
{
    if (!style.hasExplicitlySetPaddingLeft())
        style.setPaddingLeft(WTF::move(paddingBox.left()));
    if (!style.hasExplicitlySetPaddingTop())
        style.setPaddingTop(WTF::move(paddingBox.top()));
    if (!style.hasExplicitlySetPaddingRight())
        style.setPaddingRight(WTF::move(paddingBox.right()));
    if (!style.hasExplicitlySetPaddingBottom())
        style.setPaddingBottom(WTF::move(paddingBox.bottom()));
}

// A share of the font size, in whole zoomed pixels.
static Style::PaddingEdge::Fixed emPadding(const Style::ComputedStyle& style, float em)
{
    return Style::PaddingEdge::Fixed { static_cast<float>(Style::emToPxZoomed<int>(em, style)) / style.usedZoomForLength().value };
}

static void applyEmPadding(Style::ComputedStyle& style, float paddingInlineEm, float paddingBlockEm)
{
    auto paddingInline = emPadding(style, paddingInlineEm);
    auto paddingBlock = emPadding(style, paddingBlockEm);
    bool isVertical = !style.writingMode().isHorizontal();
    auto horizontalPadding = isVertical ? paddingBlock : paddingInline;
    auto verticalPadding = isVertical ? paddingInline : paddingBlock;
    applyPaddingIfNotExplicitlySet(style, { verticalPadding, horizontalPadding, verticalPadding, horizontalPadding });
}

static constexpr auto textControlInlinePaddingEm = 0.5f;
static constexpr auto textControlBlockPaddingEm = 0.25f;

// A number field keeps its stepper close to its edge.
static Style::PaddingBox paddingBoxForNumberField(const Style::ComputedStyle& style)
{
    Style::PaddingBox paddingBox { emPadding(style, textControlBlockPaddingEm) };
    paddingBox.setStart(emPadding(style, textControlInlinePaddingEm), style.writingMode());
    return paddingBox;
}

// A field with a list of suggestions is padded inside, around its text, so that the button of the
// list stays at the edge of the field.
static bool isPaddedInside(const Style::ComputedStyle& shadowHostStyle, const Element* shadowHost)
{
    RefPtr input = dynamicDowncast<HTMLInputElement>(shadowHost);
    return input && input->hasDataList() && !shadowHostStyle.nativeAppearanceDisabled();
}

// The size AppKit would give a control with text of this size.
enum class MacControlSize : uint8_t { Mini, Small, Regular };
static MacControlSize macControlSize(const Style::ComputedStyle& style, float regularFontSize)
{
    auto fontSize = style.fontDescription().computedSize();
    if (fontSize >= regularFontSize)
        return MacControlSize::Regular;
    if (fontSize >= 11)
        return MacControlSize::Small;
    return MacControlSize::Mini;
}

// The colour of the text of a button or a pop-up button: the label colour of the system, and its
// faintest shade when the control is disabled.
static void applyControlTextColor(Style::ComputedStyle& style, const Element* element)
{
    if (!element || style.hasExplicitlySetColor())
        return;
    bool isEnabled = !element->isDisabledFormControl();
    bool useDarkAppearance = protect(element->document())->styleColorOptions(&style, StyleColorOptionsPurpose::ComputedStyle).contains(StyleColorOptions::UseDarkAppearance);
    SRGBA<uint8_t> color = useDarkAppearance ? SRGBA<uint8_t> { 255, 255, 255, 216 } : SRGBA<uint8_t> { 0, 0, 0, 216 };
    if (!isEnabled)
        color = useDarkAppearance ? SRGBA<uint8_t> { 255, 255, 255, 63 } : SRGBA<uint8_t> { 0, 0, 0, 66 };
    style.setColor(Color { color, Color::Flags::Semantic });
}

int RenderThemeAdwaita::baselinePosition(const RenderBox& box) const
{
    auto appearance = box.style().usedAppearance();
    auto baseline = RenderTheme::baselinePosition(box);
    if ((appearance == StyleAppearance::Checkbox || appearance == StyleAppearance::Radio) && box.isHorizontalWritingMode())
        return baseline - 2 * box.style().usedZoom();
    return baseline;
}

void RenderThemeAdwaita::adjustButtonStyle(Style::ComputedStyle& style, const Element* element) const
{
    RenderTheme::adjustButtonStyle(style, element);
    applyControlTextColor(style, element);

    auto usedZoom = style.usedZoomForLength().value;
    auto paddingBox = Style::PaddingBox { 0_css_px, 6_css_px / usedZoom, 1_css_px / usedZoom, 6_css_px / usedZoom };
    if (!style.writingMode().isHorizontal())
        paddingBox = { paddingBox.left(), paddingBox.top(), paddingBox.right(), paddingBox.bottom() };
    applyPaddingIfNotExplicitlySet(style, paddingBox);
}

void RenderThemeAdwaita::adjustTextFieldStyle(Style::ComputedStyle& style, const Element* element) const
{
    RefPtr input = dynamicDowncast<HTMLInputElement>(element);
    if (!input || input->hasDataList())
        return;
    if (input->isNumberField())
        applyPaddingIfNotExplicitlySet(style, paddingBoxForNumberField(style));
    else
        applyEmPadding(style, textControlInlinePaddingEm, textControlBlockPaddingEm);
}

void RenderThemeAdwaita::adjustTextAreaStyle(Style::ComputedStyle& style, const Element*) const
{
    applyEmPadding(style, textControlInlinePaddingEm, textControlBlockPaddingEm);
}

void RenderThemeAdwaita::adjustSearchFieldStyle(Style::ComputedStyle& style, const Element*) const
{
    applyEmPadding(style, textControlInlinePaddingEm, textControlBlockPaddingEm);
}

void RenderThemeAdwaita::adjustTextControlInnerPlaceholderStyle(Style::ComputedStyle& style, const Style::ComputedStyle& shadowHostStyle, const Element* shadowHost) const
{
    if (isPaddedInside(shadowHostStyle, shadowHost))
        applyEmPadding(style, 0.4f, 0);
}

void RenderThemeAdwaita::adjustTextControlInnerTextStyle(Style::ComputedStyle& style, const Style::ComputedStyle& shadowHostStyle, const Element* shadowHost) const
{
    if (isPaddedInside(shadowHostStyle, shadowHost))
        applyEmPadding(style, 0.4f, 0.15f);
}

float RenderThemeAdwaita::adjustedMaximumLogicalWidthForControl(const Style::ComputedStyle& style, const Element& element, float maximumLogicalWidth) const
{
    if (!style.hasUsedAppearance() || style.nativeAppearanceDisabled())
        return maximumLogicalWidth;

    // The room a number field does not take at its end goes to its text.
    auto writingMode = style.writingMode();
    bool isInlineFlipped = writingMode.isInlineFlipped();
    bool inlineEndPaddingExplicitlySet = writingMode.isHorizontal()
        ? (isInlineFlipped ? style.hasExplicitlySetPaddingLeft() : style.hasExplicitlySetPaddingRight())
        : (isInlineFlipped ? style.hasExplicitlySetPaddingTop() : style.hasExplicitlySetPaddingBottom());
    if (RefPtr input = dynamicDowncast<HTMLInputElement>(element); input && input->isNumberField() && !inlineEndPaddingExplicitlySet) {
        auto paddingBox = paddingBoxForNumberField(style);
        auto inlineStart = paddingBox.start(writingMode).tryFixed();
        auto inlineEnd = paddingBox.end(writingMode).tryFixed();
        if (inlineStart && inlineEnd) {
            auto usedZoom = style.usedZoomForLength();
            maximumLogicalWidth += Style::evaluate<float>(*inlineStart, usedZoom) - Style::evaluate<float>(*inlineEnd, usedZoom);
        }
    }
    return maximumLogicalWidth;
}

void RenderThemeAdwaita::adjustMenuListStyle(Style::ComputedStyle& style, const Element* element) const
{
    RenderTheme::adjustMenuListStyle(style, element);
    applyControlTextColor(style, element);
    style.setLineHeight(Style::ComputedStyle::initialLineHeight());
    style.setTextAutosizingAdjustedLineHeight(Style::ComputedStyle::initialLineHeight());
}

void RenderThemeAdwaita::adjustMenuListButtonStyle(Style::ComputedStyle& style, const Element* element) const
{
    applyControlTextColor(style, element);
    float fontScale = style.usedFontSize() / 11 / style.usedZoomForLength().value;
    style.resetPadding();
    auto radius = Style::LengthPercentage<CSS::Nonnegative>::Dimension { std::trunc(5 + fontScale - 1) };
    style.setBorderRadius({ radius, radius });
    style.setMinHeight(18_css_px);
    style.setLineHeight(Style::ComputedStyle::initialLineHeight());
    style.setTextAutosizingAdjustedLineHeight(Style::ComputedStyle::initialLineHeight());
}

int RenderThemeAdwaita::minimumMenuListSize(const Style::ComputedStyle& style) const
{
    switch (macControlSize(style, 13)) {
    case MacControlSize::Regular:
        return 9 * style.usedZoom();
    case MacControlSize::Small:
        return 5 * style.usedZoom();
    case MacControlSize::Mini:
        break;
    }
    return 0;
}

Style::PaddingBox RenderThemeAdwaita::platformPopupInternalPaddingBox(const Style::ComputedStyle& style) const
{
    auto fixed = [](float value) {
        return Style::PaddingEdge::Fixed { std::trunc(value) };
    };

    // The pop-up button of AppKit: room for its arrows at the right, by the size of the control.
    if (style.usedAppearance() == StyleAppearance::Menulist) {
        switch (macControlSize(style, 16)) {
        case MacControlSize::Regular:
            return { fixed(2), fixed(26), fixed(3), fixed(8) };
        case MacControlSize::Small:
            return { fixed(2), fixed(23), fixed(3), fixed(8) };
        case MacControlSize::Mini:
            return { fixed(2), fixed(22), fixed(3), fixed(10) };
        }
    }

    // A pop-up button the page styles keeps an arrow that grows with its text.
    if (style.usedAppearance() == StyleAppearance::MenulistButton) {
        float usedZoom = style.usedZoom();
        float arrowWidth = 5 * (style.usedFontSize() / 11);
        float rightPadding = std::ceil(arrowWidth + 16 * usedZoom);
        return { fixed(1), fixed(rightPadding / usedZoom), fixed(2), fixed(8) };
    }

    return { 0_css_px };
}

#else

void RenderThemeAdwaita::adjustTextFieldStyle(Style::ComputedStyle& style, const Element*) const
{
    if (!style.hasExplicitlySetBorderRadius())
        style.setBorderRadius({ 5_css_px, 5_css_px });
}

void RenderThemeAdwaita::adjustTextAreaStyle(Style::ComputedStyle& style, const Element* element) const
{
    adjustTextFieldStyle(style, element);
}

void RenderThemeAdwaita::adjustSearchFieldStyle(Style::ComputedStyle& style, const Element* element) const
{
    adjustTextFieldStyle(style, element);
}

void RenderThemeAdwaita::adjustMenuListStyle(Style::ComputedStyle& style, const Element* element) const
{
    RenderTheme::adjustMenuListStyle(style, element);
    style.setLineHeight(Style::ComputedStyle::initialLineHeight());
    style.setTextAutosizingAdjustedLineHeight(Style::ComputedStyle::initialLineHeight());
}

void RenderThemeAdwaita::adjustMenuListButtonStyle(Style::ComputedStyle& style, const Element* element) const
{
    adjustMenuListStyle(style, element);
}

Style::PaddingBox RenderThemeAdwaita::platformPopupInternalPaddingBox(const Style::ComputedStyle& style) const
{
    if (style.usedAppearance() == StyleAppearance::None || style.usedAppearance() == StyleAppearance::Base)
        return { 0_css_px };

    auto zoomedArrowSize = menuListButtonArrowSize * style.usedZoom();

    return {
        Style::PaddingEdge::Fixed { static_cast<float>(menuListButtonPadding) },
        Style::PaddingEdge::Fixed { static_cast<float>(menuListButtonPadding + zoomedArrowSize) },
        Style::PaddingEdge::Fixed { static_cast<float>(menuListButtonPadding) },
        Style::PaddingEdge::Fixed { static_cast<float>(menuListButtonPadding) },
    };
}

#endif // OS(MACOS)

Seconds RenderThemeAdwaita::animationRepeatIntervalForProgressBar(const RenderProgress& renderer) const
{
    return renderer.page().preferredRenderingUpdateInterval();
}

IntRect RenderThemeAdwaita::progressBarRectForBounds(const RenderProgress& renderer, const IntRect& bounds) const
{
#if OS(MACOS)
    // A progress bar is as thick as its box, which the style sheet makes one em.
    UNUSED_PARAM(renderer);
    return bounds;
#else
    bool isHorizontal = renderer.isHorizontalWritingMode();
    return { bounds.x(), bounds.y(), isHorizontal ? bounds.width() : progressBarSize, isHorizontal ? progressBarSize : bounds.height() };
#endif
}

void RenderThemeAdwaita::adjustSliderThumbSize(Style::ComputedStyle& style, const Element*) const
{
    auto appearance = style.usedAppearance();
    if (appearance != StyleAppearance::SliderThumbHorizontal && appearance != StyleAppearance::SliderThumbVertical)
        return;

#if OS(MACOS)
    // The knob of a Mac's slider, which is also what makes the slider as tall as there.
    static constexpr int thumbSize = 16;
#else
    static constexpr int thumbSize = sliderThumbSize;
#endif
    style.setWidth(Style::PreferredSize::Fixed { static_cast<float>(thumbSize) });
    style.setHeight(Style::PreferredSize::Fixed { static_cast<float>(thumbSize) });
}

IntSize RenderThemeAdwaita::sliderTickSize() const
{
    return { 1, 7 };
}

int RenderThemeAdwaita::sliderTickOffsetFromTrackCenter() const
{
    return -16;
}

void RenderThemeAdwaita::adjustListButtonStyle(Style::ComputedStyle& style, const Element*) const
{
    style.setLogicalWidth(16_css_px);
    // Add a margin to place the button at end of the input field.
    style.setMarginEnd(-2_css_px);
}

Style::PreferredSizePair RenderThemeAdwaita::controlSize(StyleAppearance appearance, const FontCascade& fontCascade, const Style::PreferredSizePair& zoomedSize, float zoomFactor) const
{
    if (!zoomedSize.width().isSizingKeywordOrAuto() && !zoomedSize.height().isSizingKeywordOrAuto())
        return RenderTheme::controlSize(appearance, fontCascade, zoomedSize, zoomFactor);

    switch (appearance) {
    case StyleAppearance::Checkbox:
    case StyleAppearance::Radio: {
        auto buttonSizeWidth = zoomedSize.width();
        auto buttonSizeHeight = zoomedSize.height();
        if (buttonSizeWidth.isSizingKeywordOrAuto())
            buttonSizeWidth = 12_css_px * zoomFactor;
        if (buttonSizeHeight.isSizingKeywordOrAuto())
            buttonSizeHeight = 12_css_px * zoomFactor;
        return { WTF::move(buttonSizeWidth), WTF::move(buttonSizeHeight) };
    }
    case StyleAppearance::InnerSpinButton: {
        auto spinButtonSizeWidth = zoomedSize.width();
        auto spinButtonSizeHeight = zoomedSize.height();
        if (spinButtonSizeWidth.isSizingKeywordOrAuto())
            spinButtonSizeWidth = Style::PreferredSize::Fixed { static_cast<float>(static_cast<int>(arrowSize * zoomFactor)) };
        if (spinButtonSizeHeight.isSizingKeywordOrAuto() || fontCascade.size() > arrowSize)
            spinButtonSizeHeight = Style::PreferredSize::Fixed { fontCascade.size() };
        return { WTF::move(spinButtonSizeWidth), WTF::move(spinButtonSizeHeight) };
    }
    default:
        break;
    }

    return RenderTheme::controlSize(appearance, fontCascade, zoomedSize, zoomFactor);
}

Style::MinimumSizePair RenderThemeAdwaita::minimumControlSize(StyleAppearance appearance, const FontCascade&, const Style::MinimumSizePair& zoomedSize, float zoomFactor) const
{
    if (!zoomedSize.width().isSizingKeywordOrAuto() && !zoomedSize.height().isSizingKeywordOrAuto())
        return zoomedSize;

    auto resultWidth = zoomedSize.width();
    auto resultHeight = zoomedSize.height();

#if OS(MACOS)
    // No button of a Mac is lower than this.
    switch (appearance) {
    case StyleAppearance::PushButton:
    case StyleAppearance::DefaultButton:
    case StyleAppearance::Button:
    case StyleAppearance::SquareButton:
    case StyleAppearance::ColorWell:
        if (resultHeight.isSizingKeywordOrAuto())
            resultHeight = Style::MinimumSize::Fixed { static_cast<float>(static_cast<int>(15 * zoomFactor)) };
        break;
    default:
        break;
    }
#else
    UNUSED_PARAM(appearance);
    UNUSED_PARAM(zoomFactor);
#endif

    if (resultWidth.isSizingKeywordOrAuto())
        resultWidth = 0_css_px;
    if (resultHeight.isSizingKeywordOrAuto())
        resultHeight = 0_css_px;

    return { WTF::move(resultWidth), WTF::move(resultHeight) };
}

Style::LineWidthBox RenderThemeAdwaita::controlBorder(StyleAppearance appearance, const FontCascade& font, const Style::LineWidthBox& zoomedBox, float zoomFactor, const Element* element) const
{
    switch (appearance) {
    case StyleAppearance::PushButton:
    case StyleAppearance::DefaultButton:
    case StyleAppearance::Button:
    case StyleAppearance::SquareButton:
        return zoomedBox;
    default:
        break;
    }

    return RenderTheme::controlBorder(appearance, font, zoomedBox, zoomFactor, element);
}

#if PLATFORM(GTK) || PLATFORM(WPE)
std::optional<Seconds> RenderThemeAdwaita::caretBlinkInterval() const
{
    auto shouldBlink = SystemSettings::singleton().cursorBlink();
    auto blinkTime = SystemSettings::singleton().cursorBlinkTime();
    if (shouldBlink.value_or(true))
        return { 500_us * blinkTime.value_or(1200) };
    return { };
}
#endif // PLATFORM(GTK) || PLATFORM(WPE)

void RenderThemeAdwaita::setAccentColor(const Color& color)
{
    auto& theme = static_cast<ThemeAdwaita&>(Theme::singleton());
    if (theme.accentColor() == color)
        return;

    theme.setAccentColor(color);
    platformColorsDidChange();
}

} // namespace WebCore

#endif // USE(THEME_ADWAITA)
