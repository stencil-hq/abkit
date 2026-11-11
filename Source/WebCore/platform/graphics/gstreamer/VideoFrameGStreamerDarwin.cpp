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
#include "VideoFrameGStreamer.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)

#include "GStreamerCommon.h"
#include "GStreamerVideoFrameConverter.h"
#include "PlatformVideoColorSpace.h"
#include <CoreVideo/CVBuffer.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <gst/video/video.h>
#include <string.h>
#include <wtf/RetainPtr.h>
#include <wtf/Scope.h>
#include <wtf/cf/CFTypeTraits.h>

WTF_DECLARE_CF_TYPE_TRAIT(CVPixelBuffer);

namespace WebCore {

// The WebGPU backend reads the frame's colour space from the pixel buffer rather than being told,
// and only knows the CoreVideo names for it.
static CFStringRef colorPrimariesValue(PlatformVideoColorPrimaries primaries)
{
    switch (primaries) {
    case PlatformVideoColorPrimaries::Bt709:
        return kCVImageBufferColorPrimaries_ITU_R_709_2;
    case PlatformVideoColorPrimaries::JedecP22Phosphors:
        return kCVImageBufferColorPrimaries_EBU_3213;
    case PlatformVideoColorPrimaries::Smpte170m:
    case PlatformVideoColorPrimaries::Smpte240m:
        return kCVImageBufferColorPrimaries_SMPTE_C;
    case PlatformVideoColorPrimaries::Bt2020:
        return kCVImageBufferColorPrimaries_ITU_R_2020;
    case PlatformVideoColorPrimaries::SmpteRp431:
        return kCVImageBufferColorPrimaries_DCI_P3;
    case PlatformVideoColorPrimaries::SmpteEg432:
        return kCVImageBufferColorPrimaries_P3_D65;
    default:
        return nullptr;
    }
}

static CFStringRef transferFunctionValue(PlatformVideoTransferCharacteristics characteristics)
{
    switch (characteristics) {
    case PlatformVideoTransferCharacteristics::Bt709:
    case PlatformVideoTransferCharacteristics::Smpte170m:
        return kCVImageBufferTransferFunction_ITU_R_709_2;
    case PlatformVideoTransferCharacteristics::Smpte240m:
        return kCVImageBufferTransferFunction_SMPTE_240M_1995;
    case PlatformVideoTransferCharacteristics::SmpteSt2084:
        return kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    case PlatformVideoTransferCharacteristics::AribStdB67Hlg:
        return kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    case PlatformVideoTransferCharacteristics::Bt2020_10bit:
    case PlatformVideoTransferCharacteristics::Bt2020_12bit:
        return kCVImageBufferTransferFunction_ITU_R_2020;
    case PlatformVideoTransferCharacteristics::SmpteSt4281:
        return kCVImageBufferTransferFunction_SMPTE_ST_428_1;
    case PlatformVideoTransferCharacteristics::Iec6196621:
        return kCVImageBufferTransferFunction_sRGB;
    case PlatformVideoTransferCharacteristics::Gamma22curve:
    case PlatformVideoTransferCharacteristics::Gamma28curve:
        return kCVImageBufferTransferFunction_UseGamma;
    case PlatformVideoTransferCharacteristics::Linear:
        return kCVImageBufferTransferFunction_Linear;
    default:
        return nullptr;
    }
}

static CFStringRef yCbCrMatrixValue(PlatformVideoMatrixCoefficients matrix)
{
    switch (matrix) {
    case PlatformVideoMatrixCoefficients::Bt709:
        return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    case PlatformVideoMatrixCoefficients::Bt470bg:
    case PlatformVideoMatrixCoefficients::Smpte170m:
        return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    case PlatformVideoMatrixCoefficients::Smpte240m:
        return kCVImageBufferYCbCrMatrix_SMPTE_240M_1995;
    case PlatformVideoMatrixCoefficients::Bt2020NonconstantLuminance:
        return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    default:
        return nullptr;
    }
}

static void setColorSpaceOnPixelBuffer(CVPixelBufferRef pixelBuffer, const PlatformVideoColorSpace& colorSpace)
{
    if (colorSpace.primaries) {
        if (CFStringRef value = colorPrimariesValue(*colorSpace.primaries))
            CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey, value, kCVAttachmentMode_ShouldPropagate);
    }
    if (colorSpace.transfer) {
        if (CFStringRef value = transferFunctionValue(*colorSpace.transfer))
            CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey, value, kCVAttachmentMode_ShouldPropagate);
    }
    if (colorSpace.matrix) {
        if (CFStringRef value = yCbCrMatrixValue(*colorSpace.matrix))
            CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey, value, kCVAttachmentMode_ShouldPropagate);
    }
}

static RetainPtr<CVPixelBufferRef> createPixelBuffer(OSType pixelFormat, size_t width, size_t height)
{
    // An IOSurface-backed, Metal-compatible buffer is what lets the backend wrap the planes in
    // MTLTextures rather than reading the pixels back once more.
    RetainPtr<CFDictionaryRef> surfaceProperties = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const void* keys[] = { kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey };
    const void* values[] = { surfaceProperties.get(), kCFBooleanTrue };
    RetainPtr<CFDictionaryRef> attributes = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    CVPixelBufferRef pixelBuffer = nullptr;
    if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, pixelFormat, attributes.get(), &pixelBuffer) != kCVReturnSuccess)
        return nullptr;
    return adoptCF(pixelBuffer);
}

static bool copyPlanesToPixelBuffer(const GstVideoFrame& gstFrame, const GstVideoInfo& info, CVPixelBufferRef pixelBuffer)
{
    const GstVideoFormatInfo* formatInfo = gst_video_format_get_info(GST_VIDEO_INFO_FORMAT(&info));
    if (!formatInfo)
        return false;

    size_t planeCount = static_cast<size_t>(GST_VIDEO_FORMAT_INFO_N_PLANES(formatInfo));
    // A single-plane CVPixelBuffer reports no planes and is reached through plane 0.
    size_t destinationPlaneCount = std::max<size_t>(CVPixelBufferGetPlaneCount(pixelBuffer), 1);
    if (planeCount != destinationPlaneCount)
        return false;

    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    auto unlock = WTF::makeScopeExit([&] {
        CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    });

    for (size_t plane = 0; plane < planeCount; ++plane) {
        // The bytes of a row of a plane are those of the first component stored in that plane.
        size_t component = 0;
        for (size_t candidate = 0; candidate < GST_VIDEO_FORMAT_INFO_N_COMPONENTS(formatInfo); ++candidate) {
            if (static_cast<size_t>(GST_VIDEO_FORMAT_INFO_PLANE(formatInfo, candidate)) == plane) {
                component = candidate;
                break;
            }
        }

        auto* source = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&gstFrame, plane));
        size_t sourceStride = GST_VIDEO_FRAME_PLANE_STRIDE(&gstFrame, plane);
        auto* destination = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, plane));
        size_t destinationStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, plane);
        if (!source || !destination || !sourceStride || !destinationStride)
            return false;

        size_t rows = std::min<size_t>(GST_VIDEO_FRAME_COMP_HEIGHT(&gstFrame, component), CVPixelBufferGetHeightOfPlane(pixelBuffer, plane));
        size_t rowBytes = std::min<size_t>(GST_VIDEO_FRAME_COMP_WIDTH(&gstFrame, component) * GST_VIDEO_FRAME_COMP_PSTRIDE(&gstFrame, component), destinationStride);
        for (size_t row = 0; row < rows; ++row)
            memcpy(destination + row * destinationStride, source + row * sourceStride, rowBytes);
    }
    return true;
}

// The formats the backend has a single MTLPixelFormat for. A frame in any other one is converted
// first; YUV stays YUV so that the shader applies the frame's own matrix to it.
static bool isAcceptedFormat(GstVideoFormat format)
{
    return format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_P010_10LE || format == GST_VIDEO_FORMAT_BGRA;
}

static GstVideoFormat conversionTarget(const GstVideoFormatInfo* formatInfo)
{
    if (GST_VIDEO_FORMAT_INFO_IS_RGB(formatInfo))
        return GST_VIDEO_FORMAT_BGRA;
    if (GST_VIDEO_FORMAT_INFO_IS_YUV(formatInfo) && GST_VIDEO_FORMAT_INFO_DEPTH(formatInfo, 0) > 8)
        return GST_VIDEO_FORMAT_P010_10LE;
    return GST_VIDEO_FORMAT_NV12;
}

static OSType pixelFormatForGStreamerFormat(GstVideoFormat format, bool fullRange)
{
    switch (format) {
    case GST_VIDEO_FORMAT_NV12:
        return fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    case GST_VIDEO_FORMAT_P010_10LE:
        return fullRange ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    case GST_VIDEO_FORMAT_BGRA:
        return kCVPixelFormatType_32BGRA;
    default:
        return 0;
    }
}

CVPixelBufferRef VideoFrameGStreamer::pixelBuffer() const
{
    Locker locker { m_pixelBufferLock };
    if (m_pixelBuffer)
        return m_pixelBuffer.get();

    // An encoded frame holds no pixels of its own for the backend to wrap.
    if (isEncoded())
        return nullptr;

    GstVideoInfo info = m_info.info;
    auto format = GST_VIDEO_INFO_FORMAT(&info);
    const GstVideoFormatInfo* formatInfo = gst_video_format_get_info(format);
    if (!formatInfo || GST_VIDEO_FORMAT_INFO_IS_COMPLEX(formatInfo) || GST_VIDEO_FORMAT_INFO_IS_TILED(formatInfo))
        return nullptr;

    int width = GST_VIDEO_INFO_WIDTH(&info);
    int height = GST_VIDEO_INFO_HEIGHT(&info);
    if (width <= 0 || height <= 0)
        return nullptr;

    GRefPtr<GstSample> sample = m_sample;
    if (!isAcceptedFormat(format)) {
        GRefPtr<GstCaps> caps = adoptGRef(gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, gst_video_format_to_string(conversionTarget(formatInfo)),
            "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height,
            nullptr));
        auto converted = GStreamerVideoFrameConverter::singleton().convert(sample, caps);
        if (!converted)
            return nullptr;

        sample = WTF::move(converted);
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample.get())))
            return nullptr;
        format = GST_VIDEO_INFO_FORMAT(&info);
    }

    OSType pixelFormat = pixelFormatForGStreamerFormat(format, colorSpace().fullRange.value_or(false));
    if (!pixelFormat)
        return nullptr;

    auto pixelBuffer = createPixelBuffer(pixelFormat, width, height);
    if (!pixelBuffer)
        return nullptr;

    GstVideoFrame videoFrame;
    if (!gst_video_frame_map(&videoFrame, &info, gst_sample_get_buffer(sample.get()), GST_MAP_READ))
        return nullptr;
    auto unmap = WTF::makeScopeExit([&] {
        gst_video_frame_unmap(&videoFrame);
    });

    if (!copyPlanesToPixelBuffer(videoFrame, info, pixelBuffer.get()))
        return nullptr;

    setColorSpaceOnPixelBuffer(pixelBuffer.get(), colorSpace());

    m_pixelBuffer = WTF::move(pixelBuffer);
    return m_pixelBuffer.get();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)
