/*
 * Copyright (C) 2026 Stencil
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FontRasterizationSkiaDarwin.h"

#if USE(SKIA) && OS(DARWIN)

// Canvas text is rasterised by Core Text, as it is in Safari: the glyphs are drawn with
// CTFontDrawGlyphs into a bitmap that covers the run, with the text state FontCascadeCoreText
// gives a canvas, and the bitmap is then composited with the brush the Skia text path would have
// used. Glyph ids are font glyph indices in both stacks, so they carry over unchanged.

#include "ColorTypes.h"
#include "FontBase.h"
#include "FontCascade.h"
#include "FontPlatformData.h"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <algorithm>
#include <cmath>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RetainPtr.h>
#include <wtf/StdLibExtras.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontArguments.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkSpan.h>
#include <skia/core/SkStream.h>
#include <skia/core/SkTypeface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

// Core Graphics SPI, exported by the framework; the PAL declaration is Cocoa-only.
extern "C" void CGContextSetShouldAntialiasFonts(CGContextRef, bool shouldAntialiasFonts);

namespace WebCore {

namespace {

// A bitmap larger than this is not worth rasterising here; the run is drawn by Skia instead.
constexpr int maxGlyphRunBitmapDimension = 8192;
constexpr size_t maxGlyphRunBitmapPixels = 16 * 1024 * 1024;

CGAffineTransform toCoreGraphicsMatrix(const SkMatrix& matrix)
{
    return CGAffineTransformMake(matrix.getScaleX(), matrix.getSkewY(), matrix.getSkewX(), matrix.getScaleY(), matrix.getTranslateX(), matrix.getTranslateY());
}

CGTextDrawingMode coreTextDrawingMode(TextDrawingModeFlags mode)
{
    bool fill = mode.contains(TextDrawingMode::Fill);
    bool stroke = mode.contains(TextDrawingMode::Stroke);
    if (fill && stroke)
        return kCGTextFillStroke;
    if (stroke)
        return kCGTextStroke;
    return kCGTextFill;
}

CGLineCap coreTextLineCap(SkPaint::Cap cap)
{
    switch (cap) {
    case SkPaint::kRound_Cap:
        return kCGLineCapRound;
    case SkPaint::kSquare_Cap:
        return kCGLineCapSquare;
    case SkPaint::kButt_Cap:
        break;
    }
    return kCGLineCapButt;
}

CGLineJoin coreTextLineJoin(SkPaint::Join join)
{
    switch (join) {
    case SkPaint::kRound_Join:
        return kCGLineJoinRound;
    case SkPaint::kBevel_Join:
        return kCGLineJoinBevel;
    case SkPaint::kMiter_Join:
        break;
    }
    return kCGLineJoinMiter;
}

Lock& coreTextFontCacheLock()
{
    static Lock lock;
    return lock;
}

// The Core Text font each Skia typeface stands for, keyed by the typeface and the size. Skia gives
// every instance of a font - every variation position included - its own typeface id.
HashMap<uint64_t, RetainPtr<CTFontRef>>& coreTextFontCache()
{
    static NeverDestroyed<HashMap<uint64_t, RetainPtr<CTFontRef>>> cache;
    return cache;
}

RetainPtr<CTFontRef> createCoreTextFont(SkTypeface& typeface, float size)
{
    int ttcIndex = 0;
    auto stream = typeface.openStream(&ttcIndex);
    if (!stream)
        return nullptr;
    stream->rewind();
    auto fontData = SkData::MakeFromStream(stream.get(), stream->getLength());
    if (!fontData || !fontData->size())
        return nullptr;

    RetainPtr<CFDataRef> data = adoptCF(CFDataCreate(kCFAllocatorDefault, static_cast<const UInt8*>(fontData->data()), fontData->size()));
    RetainPtr<CTFontRef> font;
    RetainPtr<CFArrayRef> descriptors = adoptCF(CTFontManagerCreateFontDescriptorsFromData(data.get()));
    CFIndex descriptorCount = descriptors ? CFArrayGetCount(descriptors.get()) : 0;
    if (descriptorCount) {
        CFIndex index = (ttcIndex >= 0 && ttcIndex < descriptorCount) ? ttcIndex : 0;
        auto descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors.get(), index));
        font = adoptCF(CTFontCreateWithFontDescriptor(descriptor, size, nullptr));
    } else {
        RetainPtr<CGDataProviderRef> provider = adoptCF(CGDataProviderCreateWithCFData(data.get()));
        RetainPtr<CGFontRef> cgFont = adoptCF(CGFontCreateWithDataProvider(provider.get()));
        if (cgFont)
            font = adoptCF(CTFontCreateWithGraphicsFont(cgFont.get(), size, nullptr, nullptr));
    }
    if (!font)
        return nullptr;

    // The typeface may be an instance of a variable font, so the run needs the same variation
    // position; without it the glyphs would be the default instance's.
    SkFontArguments::VariationPosition::Coordinate* noCoordinates = nullptr;
    int axisCount = typeface.getVariationDesignPosition(SkSpan<SkFontArguments::VariationPosition::Coordinate>(noCoordinates, 0));
    if (axisCount > 0) {
        Vector<SkFontArguments::VariationPosition::Coordinate> coordinates(axisCount);
        if (typeface.getVariationDesignPosition(SkSpan<SkFontArguments::VariationPosition::Coordinate>(coordinates.mutableSpan().data(), coordinates.size())) == axisCount) {
            RetainPtr<CFMutableDictionaryRef> variation = adoptCF(CFDictionaryCreateMutable(kCFAllocatorDefault, axisCount, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            for (const auto& coordinate : coordinates) {
                int32_t axis = static_cast<int32_t>(coordinate.axis);
                RetainPtr<CFNumberRef> key = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &axis));
                RetainPtr<CFNumberRef> value = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat32Type, &coordinate.value));
                CFDictionarySetValue(variation.get(), key.get(), value.get());
            }
            RetainPtr<CFMutableDictionaryRef> attributes = adoptCF(CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
            CFDictionarySetValue(attributes.get(), kCTFontVariationAttribute, variation.get());
            RetainPtr<CTFontDescriptorRef> descriptor = adoptCF(CTFontDescriptorCreateWithAttributes(attributes.get()));
            RetainPtr<CTFontRef> variedFont = adoptCF(CTFontCreateCopyWithAttributes(font.get(), size, nullptr, descriptor.get()));
            if (variedFont)
                font = WTF::move(variedFont);
        }
    }

    return font;
}

RetainPtr<CTFontRef> coreTextFont(const FontPlatformData& platformData)
{
    auto* typeface = platformData.skFont().getTypeface();
    if (!typeface)
        return nullptr;

    uint64_t key = (static_cast<uint64_t>(typeface->uniqueID()) << 32) ^ static_cast<uint64_t>(std::bit_cast<uint32_t>(platformData.size()));
    Locker locker { coreTextFontCacheLock() };
    auto& cache = coreTextFontCache();
    if (auto iterator = cache.find(key); iterator != cache.end())
        return iterator->value;

    RetainPtr<CTFontRef> font = createCoreTextFont(*typeface, platformData.size());
    if (!font)
        return nullptr;
    if (cache.size() >= 128)
        cache.clear();
    cache.add(key, font);
    return font;
}

// Core Text takes glyph positions in the text matrix's coordinate system, while the advances are
// in the context's, so the pen is moved there, as FontCascadeCoreText does.
void fillGlyphPositions(Vector<CGPoint>& positions, std::span<const GlyphBufferAdvance> advances, const FloatPoint& point, CGAffineTransform inverseTextMatrix)
{
    positions[0] = CGPointApplyAffineTransform(CGPointMake(point.x(), point.y()), inverseTextMatrix);
    for (size_t i = 1; i < positions.size(); ++i) {
        CGSize advance = CGSizeApplyAffineTransform(CGSizeMake(advances[i - 1].width(), advances[i - 1].height()), inverseTextMatrix);
        positions[i] = CGPointMake(positions[i - 1].x + advance.width, positions[i - 1].y + advance.height);
    }
}

RetainPtr<CGColorRef> coreTextColor(const std::optional<Color>& color)
{
    if (!color)
        return adoptCF(CGColorCreateGenericRGB(0, 0, 0, 1));
    auto [red, green, blue, alpha] = color->toColorTypeLossy<SRGBA<float>>().resolved();
    return adoptCF(CGColorCreateGenericRGB(red, green, blue, alpha));
}

} // namespace

std::optional<CoreTextGlyphRunBitmap> rasterizeGlyphRunWithCoreText(const FontBase& font, std::span<const GlyphBufferGlyph> glyphs, std::span<const GlyphBufferAdvance> advances, const FloatPoint& position, const CoreTextGlyphRunOptions& options)
{
    if (glyphs.empty() || advances.size() < glyphs.size())
        return std::nullopt;

    const auto& platformData = font.platformData();
    // Vertical runs are laid out with Core Text's vertical metrics, which the Skia text path
    // reproduces with a transform of its own.
    if (platformData.orientation() != FontOrientation::Horizontal)
        return std::nullopt;

    RetainPtr<CTFontRef> ctFont = coreTextFont(platformData);
    if (!ctFont)
        return std::nullopt;

    // The text matrix FontCascadeCoreText sets: Core Text's y-up text space flipped into ours,
    // sheared when the font is synthetically oblique.
    CGAffineTransform textMatrix = CGAffineTransformMake(1, 0, 0, -1, 0, 0);
    if (platformData.syntheticOblique()) {
        static const float obliqueSkew = std::tan(deg2rad(FontCascade::syntheticObliqueAngle()));
        textMatrix = CGAffineTransformConcat(textMatrix, CGAffineTransformMake(1, 0, -obliqueSkew, 1, 0, 0));
    }
    CGAffineTransform inverseTextMatrix = CGAffineTransformInvert(textMatrix);

    Vector<CGPoint> positions(glyphs.size());
    fillGlyphPositions(positions, advances, position, inverseTextMatrix);

    Vector<CGRect, 16> glyphBounds(glyphs.size());
    CTFontGetBoundingRectsForGlyphs(ctFont.get(), kCTFontOrientationHorizontal, reinterpret_cast<const CGGlyph*>(glyphs.data()), glyphBounds.mutableSpan().data(), glyphs.size());
    CGRect textBounds = CGRectNull;
    for (size_t i = 0; i < glyphs.size(); ++i)
        textBounds = CGRectUnion(textBounds, CGRectOffset(glyphBounds[i], positions[i].x, positions[i].y));
    if (CGRectIsNull(textBounds))
        return std::nullopt;

    FloatSize syntheticBoldOffset;
    if (float offset = font.syntheticBoldOffset())
        syntheticBoldOffset = FloatSize(offset, 0);

    CGAffineTransform deviceMatrix = toCoreGraphicsMatrix(options.deviceMatrix);
    CGRect userBounds = CGRectApplyAffineTransform(textBounds, textMatrix);
    // Anything drawn beyond the outlines - a stroke, the second pass of synthetic bold - belongs to
    // the bitmap too.
    float gutter = syntheticBoldOffset.width();
    if (options.textDrawingMode.contains(TextDrawingMode::Stroke))
        gutter += std::max(options.strokeThickness / 2, 0.f);
    if (gutter > 0)
        userBounds = CGRectInset(userBounds, -gutter, -gutter);

    // Anti-aliasing reaches one pixel beyond the outline's bounds, and the bitmap has to be
    // pixel-aligned to be composited exactly where Core Text wrote it.
    CGRect deviceBounds = CGRectInset(CGRectApplyAffineTransform(userBounds, deviceMatrix), -1, -1);
    SkIRect deviceRect = SkIRect::MakeLTRB(static_cast<int32_t>(floorf(deviceBounds.origin.x)), static_cast<int32_t>(floorf(deviceBounds.origin.y)), static_cast<int32_t>(ceilf(CGRectGetMaxX(deviceBounds))), static_cast<int32_t>(ceilf(CGRectGetMaxY(deviceBounds))));
    if (deviceRect.isEmpty() || deviceRect.width() > maxGlyphRunBitmapDimension || deviceRect.height() > maxGlyphRunBitmapDimension)
        return std::nullopt;
    if (static_cast<size_t>(deviceRect.width()) * deviceRect.height() > maxGlyphRunBitmapPixels)
        return std::nullopt;

    // A colour font's glyphs are drawn with the colours it carries; every other glyph is painted
    // with the brush's own colour, which is only possible when the brush is a single colour.
    bool isColorBitmap = options.solidColor && platformData.isColorBitmapFont();
    size_t rowBytes = isColorBitmap ? static_cast<size_t>(deviceRect.width()) * 4 : static_cast<size_t>(deviceRect.width());
    auto pixels = SkData::MakeZeroInitialized(rowBytes * deviceRect.height());

    RetainPtr<CGColorSpaceRef> colorSpace;
    CGBitmapInfo bitmapInfo;
    if (isColorBitmap) {
        colorSpace = adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
        bitmapInfo = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little | static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst));
    } else
        bitmapInfo = static_cast<CGBitmapInfo>(kCGImageAlphaOnly);
    RetainPtr<CGContextRef> bitmapContext = adoptCF(CGBitmapContextCreate(pixels->writable_data(), deviceRect.width(), deviceRect.height(), 8, rowBytes, colorSpace.get(), bitmapInfo));
    if (!bitmapContext)
        return std::nullopt;

    // The bitmap's user space is the canvas's, moved so its device bounds start at the bitmap's
    // origin - which sits at its lower left, with y growing upwards.
    CGAffineTransform flip = CGAffineTransformMake(1, 0, 0, -1, -deviceRect.x(), deviceRect.height() + deviceRect.y());
    CGContextConcatCTM(bitmapContext.get(), CGAffineTransformConcat(deviceMatrix, flip));

    FontSmoothingMode smoothingMode = options.smoothingMode;
    if (!font.allowsAntialiasing())
        smoothingMode = FontSmoothingMode::None;
    bool shouldSmoothFonts = smoothingMode != FontSmoothingMode::Antialiased && smoothingMode != FontSmoothingMode::None;
    CGAffineTransform contextTransform = deviceMatrix;
    bool isTranslationOrIntegralScale = WTF::isIntegral(contextTransform.a) && WTF::isIntegral(contextTransform.d) && contextTransform.b == 0 && contextTransform.c == 0;
    bool isRotated = (contextTransform.b || contextTransform.c) && (contextTransform.a || contextTransform.d);

    CGContextSetShouldAntialias(bitmapContext.get(), smoothingMode != FontSmoothingMode::None);
    CGContextSetShouldSmoothFonts(bitmapContext.get(), shouldSmoothFonts);
    CGContextSetShouldAntialiasFonts(bitmapContext.get(), true);
    CGContextSetShouldSubpixelPositionFonts(bitmapContext.get(), true);
    CGContextSetShouldSubpixelQuantizeFonts(bitmapContext.get(), isTranslationOrIntegralScale || (!isRotated && options.shouldSubpixelQuantizeFonts));
    CGContextSetFontSize(bitmapContext.get(), platformData.size());
    CGContextSetTextMatrix(bitmapContext.get(), textMatrix);
    CGContextSetTextDrawingMode(bitmapContext.get(), coreTextDrawingMode(options.textDrawingMode));
    if (options.textDrawingMode.contains(TextDrawingMode::Stroke)) {
        CGContextSetLineWidth(bitmapContext.get(), options.strokeThickness);
        CGContextSetLineCap(bitmapContext.get(), coreTextLineCap(options.lineCap));
        CGContextSetLineJoin(bitmapContext.get(), coreTextLineJoin(options.lineJoin));
        CGContextSetMiterLimit(bitmapContext.get(), options.miterLimit);
        if (!options.lineDash.isEmpty()) {
            // Core Graphics wants an even number of dash lengths, as the Skia path effect is given.
            size_t dashCount = options.lineDash.size();
            Vector<CGFloat> intervals(dashCount % 2 ? dashCount * 2 : dashCount);
            for (size_t i = 0; i < intervals.size(); ++i)
                intervals[i] = options.lineDash[i % dashCount];
            CGContextSetLineDash(bitmapContext.get(), options.lineDashOffset, intervals.span().data(), intervals.size());
        }
    }
    RetainPtr<CGColorRef> color = coreTextColor(options.solidColor);
    CGContextSetFillColorWithColor(bitmapContext.get(), color.get());
    CGContextSetStrokeColorWithColor(bitmapContext.get(), color.get());

    CTFontDrawGlyphs(ctFont.get(), reinterpret_cast<const CGGlyph*>(glyphs.data()), positions.mutableSpan().data(), glyphs.size(), bitmapContext.get());
    if (syntheticBoldOffset.width()) {
        // Core Text is not asked to widen the glyphs: synthetic bold is the run drawn again, moved.
        Vector<CGPoint> boldPositions(glyphs.size());
        fillGlyphPositions(boldPositions, advances, position + syntheticBoldOffset, inverseTextMatrix);
        CTFontDrawGlyphs(ctFont.get(), reinterpret_cast<const CGGlyph*>(glyphs.data()), boldPositions.mutableSpan().data(), glyphs.size(), bitmapContext.get());
    }

    bitmapContext = nullptr;

    SkImageInfo imageInfo = isColorBitmap
        ? SkImageInfo::Make(deviceRect.width(), deviceRect.height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB())
        : SkImageInfo::MakeA8(deviceRect.width(), deviceRect.height());
    sk_sp<SkImage> image = SkImages::RasterFromData(imageInfo, WTF::move(pixels), rowBytes);
    if (!image)
        return std::nullopt;

    return CoreTextGlyphRunBitmap { WTF::move(image), SkRect::Make(deviceRect), isColorBitmap };
}

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
