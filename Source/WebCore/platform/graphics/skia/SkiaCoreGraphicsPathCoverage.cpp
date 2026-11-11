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
#include "SkiaCoreGraphicsPathCoverage.h"

#if USE(SKIA) && OS(DARWIN)

// A canvas on this port reads back Skia's anti-aliasing of a path's edge where Safari's reads
// CoreGraphics'. The difference is only the coverage of the edge, so the coverage is computed here
// with CoreGraphics, in the same terms GraphicsContextCG asks for it, and the rest of the drawing
// stays Skia's: the path is painted through the canvas' own SkPaint, and the CoreGraphics coverage
// cuts that down.

#include "AffineTransform.h"
#include "FloatRect.h"
#include "FloatRoundedRect.h"
#include "Path.h"
#include "PathImpl.h"
#include "PathSegment.h"
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/RetainPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkMatrix.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkShader.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTileMode.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// The same segment-to-CGPath conversions PathCG.cpp makes, so CoreGraphics receives the
// curves WebKit's Path would have given it: an arc stays an arc, not Skia's approximation.
static void addToCGPath(CGMutablePathRef, const PathSegment&);

static inline void addToCGPath(CGMutablePathRef path, const PathMoveTo& segment)
{
    CGPathMoveToPoint(path, nullptr, segment.point.x(), segment.point.y());
}

static inline void addToCGPath(CGMutablePathRef path, const PathLineTo& segment)
{
    CGPathAddLineToPoint(path, nullptr, segment.point.x(), segment.point.y());
}

static inline void addToCGPath(CGMutablePathRef path, const PathQuadCurveTo& segment)
{
    CGPathAddQuadCurveToPoint(path, nullptr, segment.controlPoint.x(), segment.controlPoint.y(), segment.endPoint.x(), segment.endPoint.y());
}

static inline void addToCGPath(CGMutablePathRef path, const PathBezierCurveTo& segment)
{
    CGPathAddCurveToPoint(path, nullptr, segment.controlPoint1.x(), segment.controlPoint1.y(), segment.controlPoint2.x(), segment.controlPoint2.y(), segment.endPoint.x(), segment.endPoint.y());
}

static inline void addToCGPath(CGMutablePathRef path, const PathArcTo& segment)
{
    CGPathAddArcToPoint(path, nullptr, segment.controlPoint1.x(), segment.controlPoint1.y(), segment.controlPoint2.x(), segment.controlPoint2.y(), segment.radius);
}

static inline void addToCGPath(CGMutablePathRef path, const PathArc& segment)
{
    // CG's coordinate system increases the angle in the anticlockwise direction.
    CGPathAddArc(path, nullptr, segment.center.x(), segment.center.y(), segment.radius, segment.startAngle, segment.endAngle, segment.direction == RotationDirection::Counterclockwise);
}

static inline void addToCGPath(CGMutablePathRef path, const PathClosedArc& segment)
{
    addToCGPath(path, segment.arc);
    addToCGPath(path, PathCloseSubpath { });
}

static inline AffineTransform ellipseTransform(const PathEllipse& ellipse)
{
    AffineTransform transform;
    transform.translate(ellipse.center.x(), ellipse.center.y()).rotateRadians(ellipse.rotation).scale(ellipse.radiusX, ellipse.radiusY);
    return transform;
}

static inline CGAffineTransform toCoreGraphics(const AffineTransform& transform)
{
    return CGAffineTransformMake(transform.a(), transform.b(), transform.c(), transform.d(), transform.e(), transform.f());
}

static inline CGRect toCoreGraphics(const FloatRect& rect)
{
    return CGRectMake(rect.x(), rect.y(), rect.width(), rect.height());
}

static inline void addToCGPath(CGMutablePathRef path, const PathEllipse& segment)
{
    CGAffineTransform transform = toCoreGraphics(ellipseTransform(segment));
    // CG's coordinate system increases the angle in the anticlockwise direction.
    CGPathAddArc(path, &transform, 0, 0, 1, segment.startAngle, segment.endAngle, segment.direction == RotationDirection::Counterclockwise);
}

static inline void addToCGPath(CGMutablePathRef path, const PathEllipseInRect& segment)
{
    CGPathAddEllipseInRect(path, nullptr, toCoreGraphics(segment.rect));
}

static inline void addToCGPath(CGMutablePathRef path, const PathRect& segment)
{
    CGPathAddRect(path, nullptr, toCoreGraphics(segment.rect));
}

static inline void addToCGPath(CGMutablePathRef path, const PathRoundedRect& segment)
{
    if (segment.strategy == PathRoundedRect::Strategy::PreferNative && segment.roundedRect.radii().hasEvenCorners()) {
        // As PathCG does before handing the rect to CGPathAddRoundedRect.
        auto radius = segment.roundedRect.radii().topLeft();
        CGRect rect = toCoreGraphics(segment.roundedRect.rect());
        CGFloat radiusWidth = radius.width();
        CGFloat radiusHeight = radius.height();
        if (2 * radiusWidth > rect.width())
            radiusWidth = rect.width() / 2 - std::numeric_limits<CGFloat>::epsilon();
        if (2 * radiusHeight > rect.height())
            radiusHeight = rect.height() / 2 - std::numeric_limits<CGFloat>::epsilon();
        CGPathAddRoundedRect(path, nullptr, rect, radiusWidth, radiusHeight);
        return;
    }

    for (auto& element : PathImpl::beziersForRoundedRect(segment.roundedRect))
        addToCGPath(path, element);
}

static inline void addToCGPath(CGMutablePathRef path, const PathContinuousRoundedRect& segment)
{
    addToCGPath(path, PathRoundedRect { FloatRoundedRect { segment.rect, CornerRadii { segment.cornerWidth, segment.cornerHeight } }, PathRoundedRect::Strategy::PreferNative });
}

static inline void addToCGPath(CGMutablePathRef path, const PathCloseSubpath&)
{
    CGPathCloseSubpath(path);
}

static inline void addToCGPath(CGMutablePathRef path, const PathDataLine& segment)
{
    addToCGPath(path, PathMoveTo { segment.start() });
    addToCGPath(path, PathLineTo { segment.end() });
}

static inline void addToCGPath(CGMutablePathRef path, const PathDataQuadCurve& segment)
{
    addToCGPath(path, PathMoveTo { segment.start });
    addToCGPath(path, PathQuadCurveTo { segment.controlPoint, segment.endPoint });
}

static inline void addToCGPath(CGMutablePathRef path, const PathDataBezierCurve& segment)
{
    addToCGPath(path, PathMoveTo { segment.start });
    addToCGPath(path, PathBezierCurveTo { segment.controlPoint1, segment.controlPoint2, segment.endPoint });
}

static inline void addToCGPath(CGMutablePathRef path, const PathDataArc& segment)
{
    addToCGPath(path, PathMoveTo { segment.start });
    addToCGPath(path, PathArcTo { segment.controlPoint1, segment.controlPoint2, segment.radius });
}

static void addToCGPath(CGMutablePathRef path, const PathSegment& segment)
{
    WTF::switchOn(segment.data(),
        [&](const auto& data) {
            addToCGPath(path, data);
        });
}

static RetainPtr<CGMutablePathRef> createCoreGraphicsPath(const Path& path)
{
    RetainPtr cgPath = adoptCF(CGPathCreateMutable());
    for (auto& segment : path.segments())
        addToCGPath(cgPath.get(), segment);
    return cgPath;
}

static CGAffineTransform toCoreGraphics(const SkMatrix& matrix)
{
    return CGAffineTransformMake(matrix.getScaleX(), matrix.getSkewY(), matrix.getSkewX(), matrix.getScaleY(), matrix.getTranslateX(), matrix.getTranslateY());
}

// The device pixels the coverage can differ over: the path's bounds, grown for a stroke, in the
// canvas' device space and cut to its clip.
static std::optional<SkIRect> coverageBounds(const Path& path, bool stroke, const CoreGraphicsCoverageParameters& parameters, const CGAffineTransform& deviceTransform, const SkIRect& deviceClip)
{
    RetainPtr cgPath = createCoreGraphicsPath(path);
    if (CGPathIsEmpty(cgPath.get()))
        return std::nullopt;

    CGRect bounds = CGPathGetBoundingBox(cgPath.get());
    if (stroke && parameters.lineWidth > 0) {
        // Miter joins and caps stay within this distance of the path.
        CGFloat outset = parameters.lineWidth * std::max(parameters.miterLimit, 1.0f) / 2;
        bounds = CGRectInset(bounds, -outset, -outset);
    }
    bounds = CGRectIntegral(CGRectApplyAffineTransform(bounds, deviceTransform));

    CGRect clipped = CGRectIntersection(bounds, CGRectMake(deviceClip.x(), deviceClip.y(), deviceClip.width(), deviceClip.height()));
    if (CGRectIsNull(clipped) || CGRectIsEmpty(clipped))
        return std::nullopt;

    return SkIRect::MakeXYWH(static_cast<int>(clipped.origin.x), static_cast<int>(clipped.origin.y), static_cast<int>(clipped.size.width), static_cast<int>(clipped.size.height));
}

// An 8-bit alpha image, one byte per device pixel, of how much of each pixel the path covers.
static sk_sp<SkImage> createCoverageImage(const Path& path, bool stroke, const CoreGraphicsCoverageParameters& parameters, const CGAffineTransform& deviceTransform, const SkIRect& bounds)
{
    int width = bounds.width();
    int height = bounds.height();
    if (width <= 0 || height <= 0)
        return nullptr;

    auto pixels = SkData::MakeZeroInitialized(static_cast<size_t>(width) * height);
    if (!pixels)
        return nullptr;

    RetainPtr colorSpace = adoptCF(CGColorSpaceCreateDeviceGray());
    RetainPtr context = adoptCF(CGBitmapContextCreate(pixels->writable_data(), width, height, 8, width, colorSpace.get(), kCGImageAlphaOnly));
    if (!context)
        return nullptr;

    // The bitmap's user space is the canvas's, moved so its device bounds start at the bitmap's
    // origin - which sits at its lower left, with y growing upwards. Composing the canvas' matrix
    // with the move and the flip gives this in one step.
    CGAffineTransform toBitmap = CGAffineTransformMake(deviceTransform.a, -deviceTransform.b, deviceTransform.c, -deviceTransform.d, deviceTransform.tx - bounds.x(), height + bounds.y() - deviceTransform.ty);
    CGContextConcatCTM(context.get(), toBitmap);
    CGContextSetShouldAntialias(context.get(), true);

    RetainPtr cgPath = createCoreGraphicsPath(path);
    CGContextAddPath(context.get(), cgPath.get());

    if (stroke) {
        CGContextSetLineWidth(context.get(), parameters.lineWidth);
        CGContextSetLineCap(context.get(), parameters.lineCap);
        CGContextSetLineJoin(context.get(), parameters.lineJoin);
        CGContextSetMiterLimit(context.get(), parameters.miterLimit);
        if (!parameters.dashPattern.empty()) {
            Vector<CGFloat> dashArray;
            dashArray.reserveInitialCapacity(parameters.dashPattern.size());
            for (float value : parameters.dashPattern)
                dashArray.append(value);
            CGContextSetLineDash(context.get(), parameters.dashOffset, dashArray.span().data(), dashArray.size());
        } else
            CGContextSetLineDash(context.get(), 0, nullptr, 0);
        CGContextStrokePath(context.get());
    } else if (parameters.fillRule == WindRule::EvenOdd)
        CGContextEOFillPath(context.get());
    else
        CGContextFillPath(context.get());

    context = nullptr;

    SkPixmap pixmap(SkImageInfo::MakeA8(width, height), pixels->data(), width);
    return SkImages::RasterFromPixmap(pixmap, [](const void*, void* context) {
        static_cast<SkData*>(context)->unref();
    }, pixels.release());
}

static SkSamplingOptions exactSampling()
{
    return SkSamplingOptions(SkFilterMode::kNearest);
}

bool drawPathWithCoreGraphicsCoverage(SkCanvas& canvas, const Path& path, const SkPaint& paint, bool stroke, const CoreGraphicsCoverageParameters& parameters)
{
    if (path.isEmpty())
        return false;

    // A shadow or another image filter would be applied to the coverage less faithfully than Skia
    // applies it to the path, so those keep the current path.
    if (paint.getImageFilter())
        return false;

    SkMatrix canvasMatrix = canvas.getTotalMatrix();
    CGAffineTransform deviceTransform = toCoreGraphics(canvasMatrix);

    auto bounds = coverageBounds(path, stroke, parameters, deviceTransform, canvas.getDeviceClipBounds());
    if (!bounds)
        return true;

    auto mask = createCoverageImage(path, stroke, parameters, deviceTransform, *bounds);
    if (!mask)
        return false;

    SkImageInfo layerInfo = SkImageInfo::MakeN32Premul(bounds->width(), bounds->height(), canvas.imageInfo().refColorSpace());
    auto layer = SkSurfaces::Raster(layerInfo);
    if (!layer)
        return false;

    // Paint the layer the way the canvas would have painted the path, then cut it down to the
    // coverage CoreGraphics computed.
    SkCanvas* layerCanvas = layer->getCanvas();
    SkMatrix layerMatrix = SkMatrix::Translate(-bounds->x(), -bounds->y());
    layerMatrix.preConcat(canvasMatrix);
    layerCanvas->setMatrix(layerMatrix);
    SkPaint layerPaint = paint;
    layerPaint.setBlendMode(SkBlendMode::kSrcOver);
    layerCanvas->drawPaint(layerPaint);

    layerCanvas->setMatrix(SkMatrix::I());
    SkPaint maskPaint;
    maskPaint.setBlendMode(SkBlendMode::kDstIn);
    layerCanvas->drawImage(mask, 0, 0, exactSampling(), &maskPaint);

    SkPaint compositePaint;
    compositePaint.setBlendMode(paint.getBlendMode_or(SkBlendMode::kSrcOver));
    sk_sp<SkImage> layerImage = layer->makeImageSnapshot();
    SkMatrix savedMatrix = canvas.getTotalMatrix();
    canvas.setMatrix(SkMatrix::I());
    canvas.drawImage(layerImage, bounds->x(), bounds->y(), exactSampling(), &compositePaint);
    canvas.setMatrix(savedMatrix);
    return true;
}

bool createPathCoverageClipShader(SkCanvas& canvas, const Path& path, const CoreGraphicsCoverageParameters& parameters, PathCoverageClipShader& result)
{
    result = { };

    if (path.isEmpty())
        return false;

    SkMatrix canvasMatrix = canvas.getTotalMatrix();
    SkMatrix inverseCanvasMatrix;
    if (!canvasMatrix.invert(&inverseCanvasMatrix))
        return false;
    CGAffineTransform deviceTransform = toCoreGraphics(canvasMatrix);

    auto bounds = coverageBounds(path, false, parameters, deviceTransform, canvas.getDeviceClipBounds());
    if (!bounds)
        return true;

    auto mask = createCoverageImage(path, false, parameters, deviceTransform, *bounds);
    if (!mask)
        return false;

    // Skia maps a device coordinate through the inverse of the canvas' matrix and the shader's
    // local matrix, so the local matrix is the canvas' matrix inverted and then the mask's origin
    // moved onto the image.
    SkMatrix shaderMatrix = SkMatrix::Translate(bounds->x(), bounds->y());
    shaderMatrix.postConcat(inverseCanvasMatrix);
    result.shader = mask->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, exactSampling(), shaderMatrix);
    result.coversPixels = true;
    return true;
}

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
