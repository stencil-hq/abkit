/*
 * Copyright (C) 2024 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Font.h"

#include "GlyphBuffer.h"
#include "NotImplemented.h"
#include "PathSkia.h"
#include "SkiaHarfBuzzFont.h"
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMetrics.h>
#include <wtf/StdLibExtras.h>

namespace WebCore {

#if OS(DARWIN)
// The Mac's emoji font holds its glyphs as images at a few sizes, and some glyphs have no image at
// some of them. Core Text then takes the image of another size, and measures the glyph as it does
// every other. This is the font at a size where every glyph has its image, to tell a glyph that
// lacks an image at one size from a glyph that has none at all.
static SkFont appleColorEmojiFontWithEveryImage(const SkFont& font)
{
    SkFont result = font;
    result.setSize(64);
    return result;
}

#endif

// Skia gives the box of the pixels a glyph would fill. Core Text gives the box of its outline, and
// on a Mac that is the box pages are used to seeing in the metrics of a text.
static SkRect glyphBounds(const FontPlatformData& platformData, SkGlyphID glyph)
{
    const auto& font = platformData.skFont();
#if OS(DARWIN)
    if (auto path = font.getPath(glyph))
        return path->computeTightBounds();
    // A glyph of the Mac's emoji font is an image that fills the em square above the baseline.
    if (platformData.skiaHarfBuzzFont() && platformData.skiaHarfBuzzFont()->isAppleColorEmoji()) {
        if (font.getBounds(glyph, nullptr).isEmpty() && appleColorEmojiFontWithEveryImage(font).getBounds(glyph, nullptr).isEmpty())
            return SkRect::MakeEmpty();
        return SkRect::MakeLTRB(0, -font.getSize(), font.getSize(), 0);
    }
#endif
    return font.getBounds(glyph, nullptr);
}

Path Font::platformPathForGlyph(Glyph glyph) const
{
    const auto& font = m_platformData.skFont();
    if (auto skPath = font.getPath(glyph))
        return { PathSkia::create(WTF::move(*skPath)) };

    return { };
}

FloatRect Font::platformBoundsForGlyph(Glyph glyph) const
{
    if (!m_platformData.size())
        return { };

    const auto& font = m_platformData.skFont();
    SkRect bounds = glyphBounds(m_platformData, glyph);
    bounds.offset(0, m_platformData.glyphBaselineShift());
    if (!font.isSubpixel()) {
        SkIRect rect;
        bounds.roundOut(&rect);
        bounds.set(rect);
    }
    return bounds;
}

Vector<FloatRect, Font::inlineGlyphRunCapacity> Font::platformBoundsForGlyphs(const Vector<Glyph, inlineGlyphRunCapacity>& glyphs) const
{
    if (!m_platformData.size())
        return { };

    static_assert(sizeof(Glyph) == sizeof(SkGlyphID));

    Vector<SkRect, inlineGlyphRunCapacity> bounds(glyphs.size());
    const auto& font = m_platformData.skFont();
#if OS(DARWIN)
    for (size_t i = 0; i < glyphs.size(); ++i)
        bounds[i] = glyphBounds(m_platformData, glyphs[i]);
#else
    font.getBounds(glyphs.span(), bounds.mutableSpan(), nullptr);
#endif
    return bounds.map<Vector<FloatRect, inlineGlyphRunCapacity>>([&](const auto& boundsRect) -> auto {
        SkRect returnValue = boundsRect.makeOffset(0, m_platformData.glyphBaselineShift());
        if (font.isSubpixel())
            return returnValue;
        SkIRect rect;
        returnValue.roundOut(&rect);
        returnValue.set(rect);
        return returnValue;
    });
}

float Font::platformWidthForGlyph(Glyph glyph) const
{
    if (!m_platformData.size())
        return 0;

    return SkScalarToFloat(m_platformData.glyphAdvance(glyph));
}

#if OS(DARWIN)
static std::optional<int16_t> int16InTable(const SkTypeface& typeface, SkFontTableTag tag, size_t offset)
{
    std::array<uint8_t, 2> bytes;
    if (typeface.getTableData(tag, offset, bytes.size(), bytes.data()) != bytes.size())
        return std::nullopt;
    return static_cast<int16_t>(bytes[0] << 8 | bytes[1]);
}

static constexpr SkFontTableTag os2Tag = SkSetFourByteTag('O', 'S', '/', '2');
static constexpr SkFontTableTag headTag = SkSetFourByteTag('h', 'e', 'a', 'd');
static constexpr SkFontTableTag hheaTag = SkSetFourByteTag('h', 'h', 'e', 'a');
static constexpr SkFontTableTag mathTag = SkSetFourByteTag('M', 'A', 'T', 'H');

// The line metrics of a font as Core Text and the Mac port of WebCore compute them, so that lines of
// the Mac's fonts are as tall here as in every other browser there. They are not whole pixels.
static void adjustLineMetricsToMatchCoreText(const SkFont& font, const SkString& familyName, bool isLocal, float& ascent, float& descent, float& lineGap, float& lineSpacing)
{
    auto& typeface = *font.getTypeface();
    int unitsPerEm = typeface.getUnitsPerEm();

    // Core Text takes the metrics from the 'hhea' table even when the 'OS/2' table asks for its own
    // typographic metrics to be used, which FreeType honours. WebCore makes the one exception of
    // fonts for mathematical layout, where it uses the typographic metrics too.
    static constexpr int16_t useTypographicMetrics = 1 << 7;
    auto selection = int16InTable(typeface, os2Tag, 62);
    if (unitsPerEm && selection && (*selection & useTypographicMetrics) && !typeface.getTableSize(mathTag)) {
        auto tableAscent = int16InTable(typeface, hheaTag, 4);
        auto tableDescent = int16InTable(typeface, hheaTag, 6);
        auto tableLineGap = int16InTable(typeface, hheaTag, 8);
        if (tableAscent && tableDescent && tableLineGap && (*tableAscent || *tableDescent)) {
            float scale = font.getSize() / unitsPerEm;
            ascent = *tableAscent * scale;
            descent = -*tableDescent * scale;
            lineGap = *tableLineGap * scale;
        }
    }

    // Times, Helvetica and Courier get the vertical metrics of their Microsoft counterparts, which
    // are the de facto standard of the web: 15% more, added to the ascent.
    if (isLocal && (familyName.equals("Times") || familyName.equals("Helvetica") || familyName.equals("Courier")))
        ascent += std::round((ascent + descent) * 0.15f);

    lineSpacing = lroundf(ascent) + lroundf(descent) + lroundf(lineGap);

    // The Hiragino fonts keep room under the baseline for the underlines of marked text.
    if (descent < 3 && lineGap >= 3 && familyName.startsWith("Hiragino")) {
        lineGap -= 3 - descent;
        descent = 3;
    }
}

// The height of a letter above the baseline, measured on its outline as Core Text does.
static std::optional<float> heightOfCharacter(const FontPlatformData& platformData, SkUnichar character)
{
    auto glyph = platformData.skFont().getTypeface()->unicharToGlyph(character);
    if (!glyph)
        return std::nullopt;
    return -glyphBounds(platformData, glyph).top();
}
#endif

void FontBase::platformInit()
{
    if (!m_platformData.size())
        return;

    const auto& font = m_platformData.skFont();
    SkFontMetrics metrics;
    font.getMetrics(&metrics);

    SkString familyName;
    font.getTypeface()->getFamilyName(&familyName);

#if OS(DARWIN)
    float ascent = -metrics.fAscent;
    float descent = metrics.fDescent;
    float lineGap = metrics.fLeading;
    float lineSpacing = 0;
    adjustLineMetricsToMatchCoreText(font, familyName, origin() == Origin::Local, ascent, descent, lineGap, lineSpacing);

    // FreeType measures the letter on the pixel grid when the font does not state the height.
    auto tableVersion = int16InTable(*font.getTypeface(), os2Tag, 0);
    if (!tableVersion || *tableVersion < 2 || !metrics.fCapHeight)
        metrics.fCapHeight = heightOfCharacter(m_platformData, 'H').value_or(metrics.fCapHeight);
    // The x-height is that of the letter itself, whatever the font states.
    metrics.fXHeight = heightOfCharacter(m_platformData, 'x').value_or(metrics.fXHeight);
#else
    auto ascent = SkScalarRoundToScalar(-metrics.fAscent);
    auto descent = SkScalarRoundToScalar(metrics.fDescent);
    auto lineGap = SkScalarToFloat(metrics.fLeading);
    float lineSpacing = lroundf(ascent) + lroundf(descent) + lroundf(lineGap);
#endif
    m_fontMetrics.setAscent(ascent);
    m_fontMetrics.setDescent(descent);
    m_fontMetrics.setLineGap(lineGap);
    m_fontMetrics.setLineSpacing(lineSpacing);

    m_fontMetrics.setCapHeight(metrics.fCapHeight);

    float underlinePosition;
    if (metrics.hasUnderlinePosition(&underlinePosition))
        m_fontMetrics.setUnderlinePosition(underlinePosition);
    float underlineThickness;
    if (metrics.hasUnderlineThickness(&underlineThickness))
        m_fontMetrics.setUnderlineThickness(underlineThickness);

    if (metrics.fXHeight)
        m_fontMetrics.setXHeight(metrics.fXHeight);

    m_maxCharWidth = SkScalarRoundToInt(metrics.fXMax - metrics.fXMin);
    if (metrics.fAvgCharWidth)
        m_avgCharWidth = SkScalarToFloat(metrics.fAvgCharWidth);

    m_fontMetrics.setUnitsPerEm(font.getTypeface()->getUnitsPerEm());

    // FIXME: add support for SomeEmojiGlyphs once Skia provides API for that.
    // See https://issues.skia.org/issues/374078818.
    if (m_platformData.isColorBitmapFont())
        m_emojiType = AllEmojiGlyphs { };
    else
        m_emojiType = NoEmojiGlyphs { };

    if (equalIgnoringASCIICase(familyName.c_str(), "Ahem"_s))
        m_allowsAntialiasing = false;
}

void Font::platformCharWidthInit()
{
    m_avgCharWidth = 0.f;
    m_maxCharWidth = 0.f;
#if OS(DARWIN)
    // As the Mac port of WebCore does: the average width the font states for itself, which is what
    // sizes a text field, and the width of the box around all of its glyphs.
    const auto& font = m_platformData.skFont();
    auto& typeface = *font.getTypeface();
    if (int unitsPerEm = typeface.getUnitsPerEm(); unitsPerEm && m_platformData.size()) {
        float scale = font.getSize() / unitsPerEm;
        if (auto averageWidth = int16InTable(typeface, os2Tag, 2))
            m_avgCharWidth = *averageWidth * scale;
        auto xMin = int16InTable(typeface, headTag, 36);
        auto xMax = int16InTable(typeface, headTag, 40);
        if (xMin && xMax)
            m_maxCharWidth = (*xMax - *xMin) * scale;
    }
#endif
    initCharWidths();
}

RefPtr<Font> Font::platformCreateScaledFont(const FontDescription&, float scaleFactor) const
{
    return Font::create(FontPlatformData(m_platformData.skFont().refTypeface(), scaleFactor * m_platformData.size(),
        m_platformData.syntheticBold(),
        m_platformData.syntheticOblique(),
        m_platformData.orientation(),
        m_platformData.widthVariant(),
        m_platformData.textRenderingMode(),
        Vector<hb_feature_t> { m_platformData.features() },
        m_platformData.metricsOverrides(),
        m_platformData.customPlatformData()),
        origin(), IsInterstitial::No);
}

RefPtr<Font> Font::platformCreateHalfWidthFont() const
{
    return Font::create(FontPlatformData(m_platformData.skFont().refTypeface(), m_platformData.size(),
        m_platformData.syntheticBold(),
        m_platformData.syntheticOblique(),
        m_platformData.orientation(),
        FontWidthVariant::HalfWidth,
        m_platformData.textRenderingMode(),
        Vector<hb_feature_t> { m_platformData.features() },
        m_platformData.metricsOverrides(),
        m_platformData.customPlatformData()),
        origin(), IsInterstitial::No);
}

void Font::determinePitch()
{
    m_treatAsFixedPitch = m_platformData.isFixedPitch();
}

bool Font::variantCapsSupportedForSynthesis(FontVariantCaps fontVariantCaps) const
{
    switch (fontVariantCaps) {
    case FontVariantCaps::Small:
    case FontVariantCaps::Petite:
    case FontVariantCaps::AllSmall:
    case FontVariantCaps::AllPetite:
        return false;
    default:
        // Synthesis only supports the variant-caps values listed above.
        return true;
    }
}

bool Font::platformSupportsCodePoint(char32_t character, std::optional<char32_t> variation) const
{
    if (auto* skiaHarfBuzzFont = m_platformData.skiaHarfBuzzFont())
        return !!skiaHarfBuzzFont->glyph(character, variation);

    return m_platformData.skFont().getTypeface()->unicharToGlyph(character);
}

static inline SkFont::Edging edgingForFontSmoothingMode(const SkFont& font, FontSmoothingMode smoothingMode)
{
    switch (smoothingMode) {
    case FontSmoothingMode::Auto:
        return font.getEdging();
    case FontSmoothingMode::Antialiased:
        return SkFont::Edging::kAntiAlias;
    case FontSmoothingMode::SubpixelAntialiased:
        return SkFont::Edging::kSubpixelAntiAlias;
    case FontSmoothingMode::None:
        return SkFont::Edging::kAlias;
    }

    RELEASE_ASSERT_NOT_REACHED();
}

sk_sp<SkTextBlob> FontBase::buildTextBlob(std::span<const GlyphBufferGlyph> glyphs, std::span<const GlyphBufferAdvance> advances, FontSmoothingMode smoothingMode) const
{
    if (!m_platformData.size() || !glyphs.size())
        return nullptr;

    const auto& font = m_platformData.skFont();
    auto edging = allowsAntialiasing() ? edgingForFontSmoothingMode(font, smoothingMode) : SkFont::Edging::kAlias;
    bool isVertical = m_platformData.orientation() == FontOrientation::Vertical;

    SkTextBlobBuilder builder;
    const auto& buffer = [&]() {
        if (font.getEdging() == edging)
            return isVertical ? builder.allocRunPos(font, glyphs.size()) : builder.allocRunPosH(font, glyphs.size(), m_platformData.glyphBaselineShift());

        SkFont copiedFont = font;
        copiedFont.setEdging(edging);
        return isVertical ? builder.allocRunPos(copiedFont, glyphs.size()) : builder.allocRunPosH(copiedFont, glyphs.size(), m_platformData.glyphBaselineShift());
    }();

    auto bufferGlyphs = unsafeMakeSpan<SkGlyphID>(buffer.glyphs, glyphs.size());
    auto bufferPositions = unsafeMakeSpan<SkScalar>(buffer.pos, glyphs.size() * (isVertical ? 2 : 1));

    FloatSize glyphPosition;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        bufferGlyphs[i] = glyphs[i];

        if (isVertical) {
            glyphPosition += advances[i];
            bufferPositions[2 * i] = glyphPosition.height();
            bufferPositions[2 * i + 1] = glyphPosition.width();
        } else {
            bufferPositions[i] = glyphPosition.width();
            glyphPosition += advances[i];
        }
    }

    return builder.make();
}

bool FontBase::enableAntialiasing(FontSmoothingMode smoothingMode) const
{
    return allowsAntialiasing() && edgingForFontSmoothingMode(m_platformData.skFont(), smoothingMode) != SkFont::Edging::kAlias;
}

} // namespace WebCore
