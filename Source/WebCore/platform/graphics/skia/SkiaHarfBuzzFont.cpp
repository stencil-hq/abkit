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
#include "SkiaHarfBuzzFont.h"

#if USE(SKIA)
#include "FontCache.h"
#include "FontCascadeInlines.h"
#include "FontPlatformData.h"
#include "FontTrackingSkia.h"
#include "FontVariableAdvancesSkia.h"
#include "NotImplemented.h"
#include "SkiaHarfBuzzFontCache.h"
#include <wtf/unicode/CharacterNames.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkStream.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

Ref<SkiaHarfBuzzFont> SkiaHarfBuzzFont::getOrCreate(SkTypeface& typeface)
{
    return FontCache::forCurrentThread().harfBuzzFontCache().font(typeface);
}

static hb_font_funcs_t* harfBuzzFontFunctions()
{
    static hb_font_funcs_t* fontFunctions = nullptr;

    if (!fontFunctions) {
        fontFunctions = hb_font_funcs_create();

        hb_font_funcs_set_nominal_glyph_func(fontFunctions, [](hb_font_t*, void* context, hb_codepoint_t unicode, hb_codepoint_t* glyph, void*) -> hb_bool_t {
            auto& skiaHarfBuzzFont = *static_cast<SkiaHarfBuzzFont*>(context);
            auto hbGlyph = skiaHarfBuzzFont.glyph(unicode);
            if (!hbGlyph)
                return false;
            *glyph = hbGlyph.value();
            return true;
        }, nullptr, nullptr);

        hb_font_funcs_set_variation_glyph_func(fontFunctions, [](hb_font_t*, void* context, hb_codepoint_t unicode, hb_codepoint_t variation, hb_codepoint_t* glyph, void*) -> hb_bool_t {
            auto& skiaHarfBuzzFont = *static_cast<SkiaHarfBuzzFont*>(context);
            auto hbGlyph = skiaHarfBuzzFont.glyph(unicode, variation);
            if (!hbGlyph)
                return false;
            *glyph = hbGlyph.value();
            return true;
        }, nullptr, nullptr);

        hb_font_funcs_set_glyph_h_advance_func(fontFunctions, [](hb_font_t*, void* context, hb_codepoint_t glyph, void*) -> hb_position_t {
            auto& skiaHarfBuzzFont = *static_cast<SkiaHarfBuzzFont*>(context);
            return skiaHarfBuzzFont.glyphWidth(glyph);
        }, nullptr, nullptr);

#if HB_VERSION_ATLEAST(1, 8, 6)
        hb_font_funcs_set_glyph_h_advances_func(fontFunctions, [](hb_font_t*, void* context, unsigned count, const hb_codepoint_t* glyphs, unsigned glyphStride, hb_position_t* advances, unsigned advanceStride, void*) {
            auto& skiaHarfBuzzFont = *static_cast<SkiaHarfBuzzFont*>(context);
            skiaHarfBuzzFont.glyphWidths(count, glyphs, glyphStride, advances, advanceStride);
        }, nullptr, nullptr);

#endif

        hb_font_funcs_set_glyph_extents_func(fontFunctions, [](hb_font_t*, void* context, hb_codepoint_t glyph, hb_glyph_extents_t* extents, void*) -> hb_bool_t {
            auto& skiaHarfBuzzFont = *static_cast<SkiaHarfBuzzFont*>(context);
            skiaHarfBuzzFont.glyphExtents(glyph, extents);
            return true;
        }, nullptr, nullptr);

        hb_font_funcs_make_immutable(fontFunctions);
    }
    return fontFunctions;
}

static HbUniquePtr<hb_face_t> createHarfBuzzFace(SkTypeface& typeface)
{
    int index;
    std::unique_ptr<SkStreamAsset> stream(typeface.openStream(&index));
    if (stream) {
        if (const auto* memory = stream->getMemoryBase()) {
            auto size = static_cast<unsigned>(stream->getLength());
            HbUniquePtr<hb_blob_t> blob(hb_blob_create(reinterpret_cast<const char*>(memory), size, HB_MEMORY_MODE_READONLY, stream.release(), [](void* data) {
                delete reinterpret_cast<SkStreamAsset*>(data);
            }));
            auto faceCount = hb_face_count(blob.get());
            if (faceCount && static_cast<unsigned>(index) < faceCount)
                return HbUniquePtr<hb_face_t>(hb_face_create(blob.get(), index));
        }
    }

    return HbUniquePtr<hb_face_t>(hb_face_create_for_tables([](hb_face_t*, hb_tag_t tag, void* userData) -> hb_blob_t* {
        SkTypeface& typeface = *reinterpret_cast<SkTypeface*>(userData);
        auto tableData = typeface.copyTableData(tag);
        if (!tableData)
            return nullptr;

        const auto* data = reinterpret_cast<const char*>(tableData->data());
        auto dataSize = tableData->size();
        return hb_blob_create(data, dataSize, HB_MEMORY_MODE_WRITABLE, tableData.release(), [](void* data) {
            sk_sp<SkData> tableData(reinterpret_cast<SkData*>(data));
        });
    }, &typeface, nullptr));
}

// The tracking table is applied to the advances this font hands out, the same for every way text is
// measured, so HarfBuzz must not find the table and apply it a second time to what it shapes.
static HbUniquePtr<hb_face_t> faceWithoutTrackingTable(HbUniquePtr<hb_face_t>&& face)
{
    static constexpr hb_tag_t trackingTag = HB_TAG('t', 'r', 'a', 'k');
    HbUniquePtr<hb_blob_t> table(hb_face_reference_table(face.get(), trackingTag));
    if (!hb_blob_get_length(table.get()))
        return WTF::move(face);

    unsigned index = hb_face_get_index(face.get());
    unsigned unitsPerEm = hb_face_get_upem(face.get());
    unsigned glyphCount = hb_face_get_glyph_count(face.get());
    HbUniquePtr<hb_face_t> result(hb_face_create_for_tables([](hb_face_t*, hb_tag_t tag, void* userData) -> hb_blob_t* {
        if (tag == trackingTag)
            return nullptr;
        return hb_face_reference_table(static_cast<hb_face_t*>(userData), tag);
    }, face.release(), [](void* userData) {
        hb_face_destroy(static_cast<hb_face_t*>(userData));
    }));
    hb_face_set_index(result.get(), index);
    hb_face_set_upem(result.get(), unitsPerEm);
    hb_face_set_glyph_count(result.get(), glyphCount);
    return result;
}

SkiaHarfBuzzFont::SkiaHarfBuzzFont(SkTypeface& typeface)
    : m_uniqueID(typeface.uniqueID())
    , m_isColorBitmapFont(FontPlatformData::skiaTypefaceHasAnySupportedColorTable(typeface))
{
#if OS(DARWIN)
    if (m_isColorBitmapFont) {
        SkString familyName;
        typeface.getFamilyName(&familyName);
        m_isAppleColorEmoji = familyName.equals("Apple Color Emoji") || familyName.equals(".Apple Color Emoji UI");
    }
#endif

    // Like Core Text, only for fonts that also describe their styles in a 'STAT' table, and for the
    // Mac's emoji font: older fonts carry tracking tables that nothing ever applied to running text.
    static constexpr SkFontTableTag styleAttributesTag = SkSetFourByteTag('S', 'T', 'A', 'T');
    if (m_isAppleColorEmoji || typeface.getTableSize(styleAttributesTag))
        m_tracking = FontTracking::create(typeface);

#if OS(DARWIN)
    m_unitsPerEm = typeface.getUnitsPerEm();
    if (m_unitsPerEm)
        m_variableAdvances = FontVariableAdvances::create(typeface);
#endif

    auto hbFace = faceWithoutTrackingTable(createHarfBuzzFace(typeface));
    HbUniquePtr<hb_font_t> hbFont(hb_font_create(hbFace.get()));

    if (int axisCount = typeface.getVariationDesignPosition({ }); axisCount > 0) {
        Vector<SkFontArguments::VariationPosition::Coordinate> axisValues(axisCount);
        if (typeface.getVariationDesignPosition(axisValues.mutableSpan()) > 0)
            hb_font_set_variations(hbFont.get(), reinterpret_cast<hb_variation_t*>(axisValues.mutableSpan().data()), axisValues.size());
    }

    // Create a subfont with custom functions so that the missing ones are taken from the parent.
    m_font.reset(hb_font_create_sub_font(hbFont.get()));
    hb_font_set_funcs(m_font.get(), harfBuzzFontFunctions(), this, nullptr);
}

SkiaHarfBuzzFont::~SkiaHarfBuzzFont()
{
    FontCache::forCurrentThread().harfBuzzFontCache().remove(m_uniqueID);
}

static inline hb_position_t skScalarToHarfBuzzPosition(SkScalar value)
{
    static constexpr int hbPosition = 1 << 16;
    return clampTo<int>(value * hbPosition);
}

hb_font_t* SkiaHarfBuzzFont::scaledFont(const FontPlatformData& fontPlatformData)
{
    // The size glyphs are drawn at, which is not always the size of the text.
    float size = fontPlatformData.skFont().getSize();
    auto scale = skScalarToHarfBuzzPosition(size);
    hb_font_set_scale(m_font.get(), scale, scale);
    hb_font_set_ptem(m_font.get(), size);
    m_scaledFont = fontPlatformData.skFont();
    m_scaledAdvanceAdjustment = fontPlatformData.advanceAdjustment();
    m_hasWholePixelAdvances = fontPlatformData.hasWholePixelAdvances();
    m_isColorBitmapFont = fontPlatformData.isColorBitmapFont();
    return m_font.get();
}

std::optional<hb_codepoint_t> SkiaHarfBuzzFont::glyph(hb_codepoint_t unicode, std::optional<hb_codepoint_t> variation)
{
    if (FontCascade::treatAsSpace(unicode))
        unicode = space;
    else if (FontCascade::treatAsZeroWidthSpaceInComplexScript(unicode))
        unicode = zeroWidthSpace;

    hb_codepoint_t returnValue;
    if (hb_font_get_glyph(hb_font_get_parent(m_font.get()), unicode, variation.value_or(0), &returnValue))
        return returnValue;

    if (!variation)
        return std::nullopt;

    // If we failed to get a glyph with a variation, try to get a glyph without
    // the variation unless emoji is requested and font doesn't support colors
    // or text is requested and it's a color font.
    // FIXME: it would be better to check if the font has a color/text glyph for
    // the given codepoint, but we need Skia API for that.
    // See https://issues.skia.org/issues/374078818.
    if (*variation == emojiVariationSelector && !m_isColorBitmapFont)
        return std::nullopt;
    if (*variation == textVariationSelector && m_isColorBitmapFont)
        return std::nullopt;
    if (hb_font_get_glyph(hb_font_get_parent(m_font.get()), unicode, 0, &returnValue))
        return returnValue;

    return std::nullopt;
}

SkScalar SkiaHarfBuzzFont::glyphAdvance(const SkFont& font, SkGlyphID glyph, float adjustment, bool inWholePixels) const
{
    // Skia's advance for an instance of a variable font is a whole number of design units.
    SkScalar advance = m_variableAdvances ? m_variableAdvances->advance(glyph) * font.getSize() * font.getScaleX() / m_unitsPerEm : font.getWidth(glyph);
    if (advance)
        advance += adjustment;
    if (inWholePixels)
        advance = SkScalarRoundToInt(advance);
    return advance;
}

hb_position_t SkiaHarfBuzzFont::glyphWidth(hb_codepoint_t glyph)
{
    return skScalarToHarfBuzzPosition(glyphAdvance(m_scaledFont, glyph, m_scaledAdvanceAdjustment, m_hasWholePixelAdvances));
}

void SkiaHarfBuzzFont::glyphWidths(unsigned count, const hb_codepoint_t* glyphs, unsigned glyphStride, hb_position_t* advances, unsigned advanceStride)
{
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // Glib/Win port

    Vector<SkGlyphID, 256> skGlyphs(count);
    for (unsigned i = 0; i < count; ++i) {
        skGlyphs[i] = *glyphs;
        glyphs = reinterpret_cast<const hb_codepoint_t*>(reinterpret_cast<const uint8_t*>(glyphs) + glyphStride);
    }

    Vector<SkScalar, 256> widths(count);
    if (m_variableAdvances) {
        for (unsigned i = 0; i < count; ++i)
            widths[i] = glyphAdvance(m_scaledFont, skGlyphs[i], m_scaledAdvanceAdjustment, m_hasWholePixelAdvances);
    } else {
        m_scaledFont.getWidths(skGlyphs.span(), widths.mutableSpan());
        for (unsigned i = 0; i < count; ++i) {
            if (widths[i])
                widths[i] += m_scaledAdvanceAdjustment;
            if (m_hasWholePixelAdvances)
                widths[i] = SkScalarRoundToInt(widths[i]);
        }
    }

    for (unsigned i = 0; i < count; ++i) {
        *advances = skScalarToHarfBuzzPosition(widths[i]);
        advances = reinterpret_cast<hb_position_t*>(reinterpret_cast<uint8_t*>(advances) + advanceStride);
    }

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

void SkiaHarfBuzzFont::glyphExtents(hb_codepoint_t glyph, hb_glyph_extents_t* extents)
{
    SkRect bounds = m_scaledFont.getBounds(glyph, nullptr);
    if (!m_scaledFont.isSubpixel())
        bounds.set(bounds.roundOut());

    extents->x_bearing = skScalarToHarfBuzzPosition(bounds.fLeft);
    extents->y_bearing = skScalarToHarfBuzzPosition(-bounds.fTop);
    extents->width = skScalarToHarfBuzzPosition(bounds.width());
    extents->height = skScalarToHarfBuzzPosition(-bounds.height());
}

} // namespace WebCore

#endif // USE(SKIA)
