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

#pragma once

#if USE(SKIA) && OS(DARWIN)

#include "Color.h"
#include "DashArray.h"
#include "GlyphBufferMembers.h"
#include "GraphicsTypes.h"
#include "TextFlags.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkImage.h>
#include <skia/core/SkMatrix.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkRect.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <optional>
#include <span>
#include <wtf/Forward.h>

namespace WebCore {

class FontBase;

// One glyph run rasterised by Core Text, in the device space of the Skia canvas that composites it.
struct CoreTextGlyphRunBitmap {
    // Either an alpha mask to be painted with the caller's brush, or, for colour glyphs, the pixels
    // Core Text produced.
    sk_sp<SkImage> image;
    // The device-space rectangle image covers. It is pixel-aligned, so drawing it with an identity
    // matrix reproduces the pixels Core Text wrote.
    SkRect deviceBounds;
    // Whether image carries the glyphs' own colours (colour fonts, such as emoji).
    bool isColorBitmap { false };
};

// The drawing state the rasteriser reads. It mirrors what FontCascadeCoreText sets on a Core
// Graphics context, so canvas text is rasterised as Safari rasterises it.
struct CoreTextGlyphRunOptions {
    // Canvas user space to device space.
    SkMatrix deviceMatrix;
    TextDrawingModeFlags textDrawingMode;
    float strokeThickness { 1 };
    SkPaint::Cap lineCap { SkPaint::kButt_Cap };
    SkPaint::Join lineJoin { SkPaint::kMiter_Join };
    float miterLimit { 4 };
    DashArray lineDash;
    float lineDashOffset { 0 };
    FontSmoothingMode smoothingMode { FontSmoothingMode::Auto };
    bool shouldSubpixelQuantizeFonts { true };
    // The fill or stroke colour when the brush is a single colour; a run that contains colour
    // glyphs is then rasterised in colour. When this is nullopt the brush is a gradient or an
    // image, which the caller composites itself, so the run is rasterised as an alpha mask.
    std::optional<Color> solidColor;
};

// Rasterises one glyph run with Core Text into a device-space bitmap. Returns nullopt when the run
// has to be drawn by Skia instead: vertical text, or a font with no Core Text counterpart.
std::optional<CoreTextGlyphRunBitmap> rasterizeGlyphRunWithCoreText(const FontBase&, std::span<const GlyphBufferGlyph>, std::span<const GlyphBufferAdvance>, const FloatPoint& position, const CoreTextGlyphRunOptions&);

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
