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
#include "FontCache.h"

#include "BundledRuntime.h"
#include "Font.h"
#include "FontCustomPlatformData.h"
#include "FontDescription.h"
#include "FontVariationsSkia.h"
#include "StyleFontSizeFunctions.h"
#include <wtf/Assertions.h>
#include <wtf/text/CString.h>
#include <wtf/text/CharacterProperties.h>
#include <wtf/unicode/CharacterNames.h>

#if PLATFORM(GTK) || (PLATFORM(WPE) && ENABLE(WPE_PLATFORM))
#include "SystemSettings.h"
#endif

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/ports/SkFontScanner_FreeType.h>

#if OS(ANDROID)
#include <skia/ports/SkFontMgr_android.h>
#elif PLATFORM(WIN)
#include <dwrite.h>
#include <skia/ports/SkTypeface_win.h>
#else
#include <skia/ports/SkFontMgr_fontconfig.h>
#endif
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

void FontCache::platformInit()
{
#if PLATFORM(WPE) && (OS(DARWIN) || OS(LINUX))
    // fontconfig reads its configuration on first use, which is after this.
    useBundledRuntimePathsIfPresent();
#endif
}

SkFontMgr& FontCache::fontManager() const
{
    if (!m_fontManager) {
#if OS(ANDROID)
        m_fontManager = SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
#elif PLATFORM(WIN)
        auto result = createDWriteFactory();
        m_fontManager = SkFontMgr_New_DirectWrite(result.factory.get(), result.fontCollection.get());
#else
        m_fontManager = SkFontMgr_New_FontConfig(FcConfigReference(nullptr), SkFontScanner_Make_FreeType());
#endif
    }
    RELEASE_ASSERT(m_fontManager);
    return *m_fontManager.get();
}

static SkFontStyle skiaFontStyle(const FontDescription& fontDescription)
{
    int skWeight = SkFontStyle::kNormal_Weight;
    auto weight = fontDescription.weight();
    if (weight > FontSelectionValue(SkFontStyle::kInvisible_Weight) && weight <= FontSelectionValue(SkFontStyle::kExtraBlack_Weight))
        skWeight = static_cast<int>(weight);

    int skWidth = SkFontStyle::kNormal_Width;
    auto width = fontDescription.width();
    if (width <= ultraCondensedWidthValue())
        skWidth = SkFontStyle::kUltraCondensed_Width;
    else if (width <= extraCondensedWidthValue())
        skWidth = SkFontStyle::kExtraCondensed_Width;
    else if (width <= condensedWidthValue())
        skWidth = SkFontStyle::kCondensed_Width;
    else if (width <= semiCondensedWidthValue())
        skWidth = SkFontStyle::kSemiCondensed_Width;
    else if (width >= semiExpandedWidthValue())
        skWidth = SkFontStyle::kSemiExpanded_Width;
    else if (width >= expandedWidthValue())
        skWidth = SkFontStyle::kExpanded_Width;
    if (width >= extraExpandedWidthValue())
        skWidth = SkFontStyle::kExtraExpanded_Width;
    if (width >= ultraExpandedWidthValue())
        skWidth = SkFontStyle::kUltraExpanded_Width;

    SkFontStyle::Slant skSlant = SkFontStyle::kUpright_Slant;
    if (auto fontStyleSlope = fontDescription.fontStyleSlope()) {
#if OS(MACOS)
        // As on a Mac: a slant too slight to count as italic is set in the upright face.
        if (isItalic(fontStyleSlope))
            skSlant = SkFontStyle::kItalic_Slant;
#else
        if (fontStyleSlope.value() > normalItalicValue() && fontStyleSlope.value() <= italicThreshold())
            skSlant = SkFontStyle::kItalic_Slant;
        else if (fontStyleSlope.value() > italicThreshold())
            skSlant = SkFontStyle::kOblique_Slant;
#endif
    }

    return SkFontStyle(skWeight, skWidth, skSlant);
}

static std::pair<bool, bool> computeSynthesisProperties(const SkTypeface& typeface, const FontDescription& fontDescription, OptionSet<FontLookupOptions> synthesisOptions)
{
    if (FontPlatformData::skiaTypefaceHasAnySupportedColorTable(typeface))
        return { false, false };

    bool typefaceIsBold = typeface.isBold();
#if OS(DARWIN)
    // fontconfig answers a request for a bold that a family lacks with the regular face and a note
    // to embolden it, and Skia then calls the face bold. On a Mac WebCore has to know that such a
    // bold is synthetic, because synthetic bold there also makes every advance wider.
    static constexpr SkFontTableTag os2Tag = SkSetFourByteTag('O', 'S', '/', '2');
    std::array<uint8_t, 2> weightClass;
    if (typefaceIsBold && typeface.getTableData(os2Tag, 4, weightClass.size(), weightClass.data()) == weightClass.size())
        typefaceIsBold = (weightClass[0] << 8 | weightClass[1]) >= SkFontStyle::kSemiBold_Weight;
#endif
    bool allowsSyntheticBold = fontDescription.hasAutoFontSynthesisWeight() && !synthesisOptions.contains(FontLookupOptions::DisallowBoldSynthesis);
    bool syntheticBold = allowsSyntheticBold && isFontWeightBold(fontDescription.weight()) && !typefaceIsBold;
    bool allowsSyntheticOblique = fontDescription.allowsItalicOrObliqueFontSynthesisStyle() && !synthesisOptions.contains(FontLookupOptions::DisallowObliqueSynthesis);
    bool syntheticOblique = allowsSyntheticOblique && isItalic(fontDescription.fontStyleSlope()) && !typeface.isItalic();
    return { syntheticBold, syntheticOblique };
}

RefPtr<Font> FontCache::systemFallbackForCharacterCluster(const FontDescription& description, const Font& originalFont, IsForPlatformFont, PreferColoredFont preferColoredFont, StringView stringView)
{
    auto codePoints = stringView.codePoints();
    auto codePointsIterator = codePoints.begin();
    char32_t baseCharacter = *codePointsIterator;
    ++codePointsIterator;
    if (isDefaultIgnorableCodePoint(baseCharacter) || isPrivateUseAreaCharacter(baseCharacter))
        return nullptr;

    bool isEmoji = (codePointsIterator != codePoints.end() && *codePointsIterator == emojiVariationSelector) || preferColoredFont == PreferColoredFont::Yes;

#if OS(ANDROID) || PLATFORM(WIN)
    // FIXME: handle locale.
    Vector<const char*, 1> bcp47;
    if (isEmoji)
        bcp47.append("und-Zsye");
    auto typeface = fontManager().matchFamilyStyleCharacter(nullptr, { }, bcp47.mutableSpan().data(), bcp47.size(), baseCharacter);
#else
    auto typeface = m_skiaSystemFallbackFontCache.fontForCharacterCluster(isEmoji ? "und-Zsye"_s : description.usedLocale(), stringView);
#endif
    if (!typeface)
        return nullptr;

#if OS(MACOS)
    // In text set in one of the Mac's interface fonts, emoji come from the interface variant of the
    // emoji font, whose glyphs stand further apart.
    if (auto originalFamily = originalFont.platformData().familyName(); originalFamily.startsWith('.') || originalFamily == "System Font"_s) {
        SkString fallbackFamily;
        typeface->getFamilyName(&fallbackFamily);
        if (fallbackFamily.equals("Apple Color Emoji")) {
            if (auto interfaceEmoji = fontManager().matchFamilyStyle(".Apple Color Emoji UI", typeface->fontStyle()))
                typeface = WTF::move(interfaceEmoji);
        }
    }
#else
    UNUSED_PARAM(originalFont);
#endif

    auto features = computeFeatures(description, { });
    auto [syntheticBold, syntheticOblique] = computeSynthesisProperties(*typeface, description, { });

    // @font-face size-adjust does not affect fallback font sizes, but font-size-adjust does.
    // We initialize FontPlatformData with the computed size, then apply font-size-adjust if required.
    auto size = description.usedSize();
    FontPlatformData alternateFontData(WTF::move(typeface), size, syntheticBold, syntheticOblique, description.orientation(), description.widthVariant(), description.textRenderingMode(), WTF::move(features));
    alternateFontData.updateSizeWithFontSizeAdjust(description.fontSizeAdjust(), size);

    return fontForPlatformData(alternateFontData);
}

Vector<String> FontCache::systemFontFamilies()
{
    auto& manager = fontManager();
    int count = manager.countFamilies();
    Vector<String> fontFamilies(ReserveCapacityTag { }, count);
    for (int i = 0; i < count; ++i) {
        SkString familyName;
        manager.getFamilyName(i, &familyName);
        fontFamilies.append(String::fromUTF8(familyName.data()));
    }
    return fontFamilies;
}

bool FontCache::isSystemFontForbiddenForEditing(const String&)
{
    return false;
}

Ref<Font> FontCache::lastResortFallbackFont(const FontDescription& fontDescription)
{
#if PLATFORM(WIN)
    const auto defaultFontName = "Times New Roman"_s;
#else
    const auto defaultFontName = "serif"_s;
#endif
    if (RefPtr<Font> font = fontForFamily(fontDescription, defaultFontName))
        return font.releaseNonNull();

    // Passing nullptr as family name makes Skia use a weak match.
    auto typeface = fontManager().matchFamilyStyle(nullptr, skiaFontStyle(fontDescription));
    if (!typeface) {
        // LastResort is guaranteed to be non-null, so fallback to empty font with not glyphs.
        typeface = SkTypeface::MakeEmpty();
    }

    auto [syntheticBold, syntheticOblique] = computeSynthesisProperties(*typeface, fontDescription, { });
    FontPlatformData platformData(WTF::move(typeface), fontDescription.usedSize(), syntheticBold, syntheticOblique,
        fontDescription.orientation(), fontDescription.widthVariant(), fontDescription.textRenderingMode(), computeFeatures(fontDescription, { }));
    return fontForPlatformData(platformData);
}

Vector<FontSelectionCapabilities> FontCache::getFontSelectionCapabilitiesInFamily(const AtomString&, AllowUserInstalledFonts)
{
    return { };
}

#if PLATFORM(GTK) || (PLATFORM(WPE) && ENABLE(WPE_PLATFORM))
static bool isSystemUIFamilyFont(const String& family)
{
    return family == "-webkit-system-font"_s
        || family == "-webkit-system-ui"_s
        || family == "system-font"_s
        || family == "system-ui"_s
#if OS(MACOS)
        || family == "-apple-system"_s
        || family == "-apple-menu"_s
        || family == "-apple-status-bar"_s
        || family == "-apple-system-monospaced-numbers"_s
#endif
        || family == "ui-sans-serif"_s;
}
#endif

#if OS(MACOS)
// The other interface fonts of a Mac, under the names fontconfig knows them by.
static ASCIILiteral macInterfaceFontFamily(const String& family)
{
    if (family == "ui-serif"_s)
        return ".New York"_s;
    if (family == "ui-monospace"_s)
        return ".SF NS Mono"_s;
    if (family == "ui-rounded"_s)
        return ".SF NS Rounded"_s;
    return { };
}
#endif

static String getFamilyNameStringFromFamily(const String& family)
{
#if PLATFORM(GTK) || (PLATFORM(WPE) && ENABLE(WPE_PLATFORM))
    if (isSystemUIFamilyFont(family))
        return SystemSettings::singleton().defaultSystemFont();
#endif
#if OS(MACOS)
    if (auto interfaceFamily = macInterfaceFontFamily(family); !interfaceFamily.isNull())
        return interfaceFamily;
#endif

    // If we're creating a fallback font (e.g. "-webkit-monospace"), convert the name into
    // the fallback name (like "monospace") that fontconfig understands.
    if (family.length() && !family.startsWith("-webkit-"_s))
        return family;

    if (family == *familyNamesData->at(FamilyNamesIndex::StandardFamily) || family == *familyNamesData->at(FamilyNamesIndex::SerifFamily))
        return "serif"_s;
    if (family == *familyNamesData->at(FamilyNamesIndex::SansSerifFamily))
        return "sans-serif"_s;
    if (family == *familyNamesData->at(FamilyNamesIndex::MonospaceFamily))
        return "monospace"_s;
    if (family == *familyNamesData->at(FamilyNamesIndex::CursiveFamily))
        return "cursive"_s;
    if (family == *familyNamesData->at(FamilyNamesIndex::FantasyFamily))
        return "fantasy"_s;
    if (family == *familyNamesData->at(FamilyNamesIndex::MathFamily))
        return "math"_s;

    return emptyString();
}

Vector<hb_feature_t> FontCache::computeFeatures(const FontDescription& fontDescription, const FontCreationContext& fontCreationContext)
{
    FeaturesMap featuresToBeApplied;

    // 7.2. Feature precedence
    // https://www.w3.org/TR/css-fonts-3/#feature-precedence

    // 1. Font features enabled by default, including features required for a given script.

    // FIXME: optical sizing.

    // 2. If the font is defined via an @font-face rule, the font features implied by the
    //    font-feature-settings descriptor in the @font-face rule.
    if (fontCreationContext.fontFaceFeatures()) {
        for (auto& fontFaceFeature : *fontCreationContext.fontFaceFeatures())
            featuresToBeApplied.set(fontFaceFeature.tag(), fontFaceFeature.value());
    }

    // 3. Font features implied by the value of the ‘font-variant’ property, the related ‘font-variant’
    //    subproperties and any other CSS property that uses OpenType features.
    for (auto& newFeature : computeFeatureSettingsFromVariants(fontDescription.variantSettings(), fontCreationContext.fontFeatureValues()))
        featuresToBeApplied.set(newFeature.key, newFeature.value);

    // 4. Feature settings determined by properties other than ‘font-variant’ or ‘font-feature-settings’.
    bool optimizeSpeed = fontDescription.textRenderingMode() == TextRenderingMode::OptimizeSpeed;
    bool shouldDisableLigaturesForSpacing = fontDescription.shouldDisableLigaturesForSpacing();

    // clig and liga are on by default in HarfBuzz.
    auto commonLigatures = fontDescription.variantCommonLigatures();
    if (shouldDisableLigaturesForSpacing || (commonLigatures == FontVariantLigatures::No || (commonLigatures == FontVariantLigatures::Normal && optimizeSpeed))) {
        featuresToBeApplied.set(fontFeatureTag("liga"), 0);
        featuresToBeApplied.set(fontFeatureTag("clig"), 0);
    }

    if (shouldDisableLigaturesForSpacing) {
        featuresToBeApplied.set(fontFeatureTag("dlig"), 0);
        featuresToBeApplied.set(fontFeatureTag("hlig"), 0);
    } else {
        auto discretionaryLigatures = fontDescription.variantDiscretionaryLigatures();
        if (discretionaryLigatures == FontVariantLigatures::Yes)
            featuresToBeApplied.set(fontFeatureTag("dlig"), 1);

        auto historicalLigatures = fontDescription.variantHistoricalLigatures();
        if (historicalLigatures == FontVariantLigatures::Yes)
            featuresToBeApplied.set(fontFeatureTag("hlig"), 1);
    }

    // calt is on by default in HarfBuzz.
    auto contextualAlternates = fontDescription.variantContextualAlternates();
    if (shouldDisableLigaturesForSpacing || (contextualAlternates == FontVariantLigatures::No || (contextualAlternates == FontVariantLigatures::Normal && optimizeSpeed)))
        featuresToBeApplied.set(fontFeatureTag("calt"), 0);

    switch (fontDescription.widthVariant()) {
    case FontWidthVariant::RegularWidth:
        break;
    case FontWidthVariant::HalfWidth:
        featuresToBeApplied.set(fontFeatureTag("hwid"), 1);
        break;
    case FontWidthVariant::ThirdWidth:
        featuresToBeApplied.set(fontFeatureTag("twid"), 1);
        break;
    case FontWidthVariant::QuarterWidth:
        featuresToBeApplied.set(fontFeatureTag("qwid"), 1);
        break;
    }

    switch (fontDescription.variantEastAsianVariant()) {
    case FontVariantEastAsianVariant::Normal:
        break;
    case FontVariantEastAsianVariant::Jis78:
        featuresToBeApplied.set(fontFeatureTag("jp78"), 1);
        break;
    case FontVariantEastAsianVariant::Jis83:
        featuresToBeApplied.set(fontFeatureTag("jp83"), 1);
        break;
    case FontVariantEastAsianVariant::Jis90:
        featuresToBeApplied.set(fontFeatureTag("jp90"), 1);
        break;
    case FontVariantEastAsianVariant::Jis04:
        featuresToBeApplied.set(fontFeatureTag("jp04"), 1);
        break;
    case FontVariantEastAsianVariant::Simplified:
        featuresToBeApplied.set(fontFeatureTag("smpl"), 1);
        break;
    case FontVariantEastAsianVariant::Traditional:
        featuresToBeApplied.set(fontFeatureTag("trad"), 1);
        break;
    }

    switch (fontDescription.variantEastAsianWidth()) {
    case FontVariantEastAsianWidth::Normal:
        break;
    case FontVariantEastAsianWidth::Full:
        featuresToBeApplied.set(fontFeatureTag("fwid"), 1);
        break;
    case FontVariantEastAsianWidth::Proportional:
        featuresToBeApplied.set(fontFeatureTag("pwid"), 1);
        break;
    }

    switch (fontDescription.variantEastAsianRuby()) {
    case FontVariantEastAsianRuby::Normal:
        break;
    case FontVariantEastAsianRuby::Yes:
        featuresToBeApplied.set(fontFeatureTag("ruby"), 1);
        break;
    }

    switch (fontDescription.variantNumericFigure()) {
    case FontVariantNumericFigure::Normal:
        break;
    case FontVariantNumericFigure::LiningNumbers:
        featuresToBeApplied.set(fontFeatureTag("lnum"), 1);
        break;
    case FontVariantNumericFigure::OldStyleNumbers:
        featuresToBeApplied.set(fontFeatureTag("onum"), 1);
        break;
    }

    switch (fontDescription.variantNumericSpacing()) {
    case FontVariantNumericSpacing::Normal:
        break;
    case FontVariantNumericSpacing::ProportionalNumbers:
        featuresToBeApplied.set(fontFeatureTag("pnum"), 1);
        break;
    case FontVariantNumericSpacing::TabularNumbers:
        featuresToBeApplied.set(fontFeatureTag("tnum"), 1);
        break;
    }

    switch (fontDescription.variantNumericFraction()) {
    case FontVariantNumericFraction::Normal:
        break;
    case FontVariantNumericFraction::DiagonalFractions:
        featuresToBeApplied.set(fontFeatureTag("frac"), 1);
        break;
    case FontVariantNumericFraction::StackedFractions:
        featuresToBeApplied.set(fontFeatureTag("afrc"), 1);
        break;
    }

    if (fontDescription.variantNumericOrdinal() == FontVariantNumericOrdinal::Yes)
        featuresToBeApplied.set(fontFeatureTag("ordn"), 1);

    if (fontDescription.variantNumericSlashedZero() == FontVariantNumericSlashedZero::Yes)
        featuresToBeApplied.set(fontFeatureTag("zero"), 1);

    // 5. Font features implied by the value of ‘font-feature-settings’ property.
    for (auto& newFeature : fontDescription.featureSettings())
        featuresToBeApplied.set(newFeature.tag(), newFeature.value());

    if (featuresToBeApplied.isEmpty())
        return { };

    Vector<hb_feature_t> features(ReserveCapacityTag { }, featuresToBeApplied.size());
    for (const auto& iter : featuresToBeApplied)
        features.append({ HB_TAG(iter.key[0], iter.key[1], iter.key[2], iter.key[3]), static_cast<uint32_t>(iter.value), 0, static_cast<unsigned>(-1) });
    return features;
}

static bool isUnsupportedFamilyFont(const AtomString& family)
{
#if OS(MACOS)
    // `-apple-system` names the system font on a Mac, to this engine as to every other browser
    // there. The fonts whose names start with a dot are the system's own: a page reaches them
    // through `system-ui` and the `ui-` families, never by name.
    return family.startsWith('.');
#else
    return family == "-apple-system"
        || family == "-apple-system-font";
#endif
}

// An installed variable font as Core Text sets it up for a style: every axis the font has follows the
// style or the page's `font-variation-settings`, the way the axes of a web font do.
struct InstalledFontInstance {
    sk_sp<SkTypeface> typeface;
    // The text size the font is spaced for when that is not the size of the text.
    float trackingSize { 0 };
    // Whether an axis of the font gives the weight, or the slant, the style asks for.
    bool hasWeightAxis { false };
    bool hasSlopeAxis { false };
};

static InstalledFontInstance instantiateInstalledFont(sk_sp<SkTypeface>&& typeface, const FontDescription& fontDescription, float size)
{
    auto axes = defaultFontVariationValues(*typeface);
    if (axes.isEmpty())
        return { WTF::move(typeface) };

    // The axes this code does not set stay where the match for the requested style put them.
    Vector<SkFontArguments::VariationPosition::Coordinate> position;
    if (int count = typeface->getVariationDesignPosition({ }); count > 0) {
        position.grow(count);
        if (typeface->getVariationDesignPosition(position.mutableSpan()) <= 0)
            position.clear();
    }
    auto original = position;

    auto apply = [&](const FontTag& tag, float value) {
        auto axis = axes.find(tag);
        if (axis == axes.end())
            return;
        auto skiaTag = SkSetFourByteTag(tag[0], tag[1], tag[2], tag[3]);
        position.removeAllMatching([&](auto& coordinate) {
            return coordinate.axis == skiaTag;
        });
        position.append({ skiaTag, axis->value.clamp(value) });
    };

    // Fonts older than OpenType variations (TrueType GX) count weight and width around 1, not on the
    // CSS scales; the conversions are the ones WebCore uses with Core Text.
    static constexpr SkFontTableTag styleAttributesTag = SkSetFourByteTag('S', 'T', 'A', 'T');
    bool usesCSSScales = typeface->getTableSize(styleAttributesTag);
    float weight = fontDescription.weight();
    float width = fontDescription.width();
    float slope = fontDescription.fontStyleSlope().value_or(normalItalicValue());
    if (!usesCSSScales) {
        weight = (weight + 109.3f) / 523.7f;
        if (width <= 125)
            width /= 100;
        else if (width <= 150)
            width = (width + 125) / 200;
        else
            width = (width + 400) / 400;
        slope /= 300;
    }
    apply(FontVariationAxisTag::wght, weight);
    apply(FontVariationAxisTag::wdth, width);
    if (fontDescription.fontStyleAxis() == FontStyleAxis::ital)
        apply(FontVariationAxisTag::ital, 1);
    else {
        // The 'slnt' axis is positive counter-clockwise; a CSS oblique angle is positive clockwise.
        apply(FontVariationAxisTag::slnt, -slope);
    }

    // Optical sizing. On `auto` the font is drawn at the optical size of the text unless the page
    // sets that axis itself. On `none` it is drawn and spaced as at its default optical size at
    // every size of text, and Core Text takes a setting that only repeats the automatic value as
    // no setting at all. `text-rendering: optimizeLegibility` turns optical sizing on regardless.
    float trackingSize = 0;
    auto opticalSizeAxis = axes.find(FontVariationAxisTag::opsz);
    bool sizesOptically = fontDescription.opticalSizing() == FontOpticalSizing::Auto || fontDescription.textRenderingMode() == TextRenderingMode::OptimizeLegibility;
    if (opticalSizeAxis != axes.end()) {
        if (sizesOptically)
            apply(FontVariationAxisTag::opsz, size);
        else {
            apply(FontVariationAxisTag::opsz, opticalSizeAxis->value.defaultValue);
            trackingSize = opticalSizeAxis->value.defaultValue;
        }
    }

    // One more thing Core Text does on `none`: for text no larger than the smallest optical size of
    // the font, it takes no setting at all unless the settings move the optical size.
    bool takesSettings = true;
    if (!sizesOptically && opticalSizeAxis != axes.end() && size <= opticalSizeAxis->value.minimumValue) {
        takesSettings = std::ranges::any_of(fontDescription.variationSettings(), [&](auto& variation) {
            return variation.tag() == FontVariationAxisTag::opsz && opticalSizeAxis->value.clamp(variation.value()) != opticalSizeAxis->value.minimumValue;
        });
    }
    for (auto& variation : fontDescription.variationSettings()) {
        if (!takesSettings)
            break;
        if (!sizesOptically && variation.tag() == FontVariationAxisTag::opsz && opticalSizeAxis != axes.end() && opticalSizeAxis->value.clamp(variation.value()) == opticalSizeAxis->value.clamp(size))
            continue;
        apply(variation.tag(), variation.value());
    }

    InstalledFontInstance instance;
    instance.trackingSize = trackingSize;
    // Core Text does not see the weight of an instance of the older fonts, so WebCore emboldens
    // their bold on top of the axis.
    instance.hasWeightAxis = usesCSSScales && axes.contains(FontVariationAxisTag::wght);
    instance.hasSlopeAxis = axes.contains(FontVariationAxisTag::slnt) || axes.contains(FontVariationAxisTag::ital);

    auto valueOf = [](const auto& coordinates, SkFourByteTag tag) -> std::optional<float> {
        for (auto& coordinate : coordinates) {
            if (coordinate.axis == tag)
                return coordinate.value;
        }
        return std::nullopt;
    };
    bool changed = std::ranges::any_of(position, [&](auto& coordinate) {
        return valueOf(original, coordinate.axis) != coordinate.value;
    });
    if (changed) {
        SkFontArguments arguments;
        arguments.setVariationDesignPosition({ position.span().data(), static_cast<int>(position.size()) });
        if (auto clone = typeface->makeClone(arguments)) {
            instance.typeface = WTF::move(clone);
            return instance;
        }
    }
    instance.typeface = WTF::move(typeface);
    return instance;
}

std::unique_ptr<FontPlatformData> FontCache::createFontPlatformData(const FontDescription& fontDescription, const AtomString& family, const FontCreationContext& fontCreationContext, OptionSet<FontLookupOptions> options)
{
    if (isUnsupportedFamilyFont(family))
        return nullptr;
    auto familyName = getFamilyNameStringFromFamily(family);
    auto skFontStyle = skiaFontStyle(fontDescription);
    auto typeface = fontManager().matchFamilyStyle(familyName.utf8().legacyCStringPointer(), skFontStyle);
    if (!typeface)
        return nullptr;

#if OS(MACOS)
    // The italic of the Mac's system font has no width axis. Core Text answers a request for a
    // condensed or expanded italic with the upright face at that width.
    if (skFontStyle.slant() != SkFontStyle::kUpright_Slant && fontDescription.width() != normalWidthValue() && familyName != family.string() && !defaultFontVariationValues(*typeface).contains(FontVariationAxisTag::wdth)) {
        if (auto upright = fontManager().matchFamilyStyle(familyName.utf8().legacyCStringPointer(), SkFontStyle(skFontStyle.weight(), skFontStyle.width(), SkFontStyle::kUpright_Slant)); upright && defaultFontVariationValues(*upright).contains(FontVariationAxisTag::wdth))
            typeface = WTF::move(upright);
    }
#endif

    auto size = fontDescription.adjustedSizeForFontFace(fontCreationContext.sizeAdjust());
    auto instance = instantiateInstalledFont(WTF::move(typeface), fontDescription, size);
    typeface = WTF::move(instance.typeface);
    auto features = computeFeatures(fontDescription, fontCreationContext);
#if OS(MACOS)
    if (family == "-apple-system-monospaced-numbers"_s)
        features.append({ HB_TAG('t', 'n', 'u', 'm'), 1, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END });
#endif
    auto [syntheticBold, syntheticOblique] = computeSynthesisProperties(*typeface, fontDescription, options);
    // An axis gives the weight or the slant that was asked for; nothing is left to imitate.
    if (instance.hasWeightAxis)
        syntheticBold = false;
    if (instance.hasSlopeAxis)
        syntheticOblique = false;
    FontPlatformData platformData(WTF::move(typeface), size, syntheticBold, syntheticOblique, fontDescription.orientation(), fontDescription.widthVariant(), fontDescription.textRenderingMode(), WTF::move(features), fontCreationContext.metricsOverrides());
    if (instance.trackingSize)
        platformData.setTrackingSize(instance.trackingSize);

    platformData.updateSizeWithFontSizeAdjust(fontDescription.fontSizeAdjust(), fontDescription.usedSize());
    auto platformDataUniquePtr = makeUnique<FontPlatformData>(platformData);

    return platformDataUniquePtr;
}

ASCIILiteral FontCache::platformAlternateFamilyName(const String&)
{
    return { };
}

void FontCache::platformReleaseNoncriticalMemory()
{
    for (auto& entry : m_fontCascadeCache.m_entries.values()) {
        entry->fonts->forEachRealizedFont([](const Font& font) {
            if (const auto* customPlatformData = font.platformData().customPlatformData())
                customPlatformData->clearVariationTypefacesCache();
        });
    }
}

void FontCache::platformInvalidate()
{
}

void FontCache::platformPurgeInactiveFontData()
{
    m_harfBuzzFontCache.clear();
#if !OS(ANDROID) && !PLATFORM(WIN)
    m_skiaSystemFallbackFontCache.clear();
#endif
}

} // namespace WebCore
