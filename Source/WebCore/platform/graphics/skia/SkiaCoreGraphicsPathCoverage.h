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

#include "WindRule.h"
#include <CoreGraphics/CoreGraphics.h>
#include <span>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkShader.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

class SkCanvas;
class SkPaint;
class SkShader;

namespace WebCore {

class Path;

// The geometry CoreGraphics needs to cover the edge of a path, in the same terms
// GraphicsContextCG puts them: the fill rule, or the stroke's width, cap, join,
// miter limit and dash.
struct CoreGraphicsCoverageParameters {
    // Fill rule of a fill or a clip.
    WindRule fillRule { WindRule::NonZero };

    // Stroke geometry, used when the path is stroked.
    float lineWidth { 0 };
    CGLineCap lineCap { kCGLineCapButt };
    CGLineJoin lineJoin { kCGLineJoinMiter };
    float miterLimit { 4 };
    std::span<const float> dashPattern;
    float dashOffset { 0 };
};

// Draws path on canvas with the edge coverage CoreGraphics gives it, so a canvas
// on this port reads back the pixels Safari's does. Skia still paints the colour,
// shader, alpha and blend mode of paint; only the coverage of the edge is
// CoreGraphics'. Returns false without drawing when the coverage cannot be
// computed, and the caller draws the path with Skia as before. When stroke is
// true the path is stroked with parameters; otherwise it is filled with them.
bool drawPathWithCoreGraphicsCoverage(SkCanvas&, const Path&, const SkPaint&, bool stroke, const CoreGraphicsCoverageParameters&);

// The coverage of a path to clip with: the shader is in device space, so the caller
// clips with an identity canvas matrix and records it for state replay.
struct PathCoverageClipShader {
    sk_sp<SkShader> shader;
    // False when the path covers no pixel of the canvas' clip, in which case shader is
    // null and intersecting with the path empties the clip.
    bool coversPixels { false };
};

// Computes the clip shader for path, so a canvas clip reads back the coverage
// Safari's does. Returns false when the coverage cannot be computed.
bool createPathCoverageClipShader(SkCanvas&, const Path&, const CoreGraphicsCoverageParameters&, PathCoverageClipShader&);

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
