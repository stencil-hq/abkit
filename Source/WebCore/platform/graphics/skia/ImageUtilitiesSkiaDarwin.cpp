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
#include "ImageUtilities.h"

#if USE(SKIA) && OS(DARWIN)

// Encoded images (canvas.toDataURL(), toBlob(), convertToBlob()) come from ImageIO on Darwin, as in
// Safari: a page hashing them gets the bytes Safari gives for the same pixels. The images handed to
// ImageIO are laid out as the Cocoa ports' are (ImageUtilitiesCG.cpp), since the layout decides how
// ImageIO converts them.

#include "GLContext.h"
#include "MIMETypeRegistry.h"
#include "NativeImage.h"
#include "PixelBuffer.h"
#include "PlatformDisplay.h"
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurface.h>
#include <ImageIO/ImageIO.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MallocSpan.h>
#include <wtf/RetainPtr.h>
#include <wtf/text/Base64.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <skia/encode/SkICC.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// The type identifiers of the types MIMETypeRegistry lists for encoding. Safari lists AVIF and HEIC
// too, but its web process cannot encode them: a canvas asked for one gives no data there ("data:,"
// from toDataURL(), null from toBlob()) instead of falling back to PNG, so they have no identifier here.
static CFStringRef typeIdentifierForMIMEType(const String& mimeType)
{
    if (equalLettersIgnoringASCIICase(mimeType, "image/png"_s))
        return CFSTR("public.png");
    if (equalLettersIgnoringASCIICase(mimeType, "image/jpeg"_s))
        return CFSTR("public.jpeg");
    if (equalLettersIgnoringASCIICase(mimeType, "image/gif"_s))
        return CFSTR("com.compuserve.gif");
    if (equalLettersIgnoringASCIICase(mimeType, "image/tiff"_s))
        return CFSTR("public.tiff");
    if (equalLettersIgnoringASCIICase(mimeType, "image/bmp"_s))
        return CFSTR("com.microsoft.bmp");
    if (equalLettersIgnoringASCIICase(mimeType, "image/vnd.microsoft.icon"_s))
        return CFSTR("com.microsoft.ico");
    return nullptr;
}

static RetainPtr<CGColorSpaceRef> cgColorSpace(SkColorSpace* colorSpace)
{
    if (!colorSpace || colorSpace->isSRGB())
        return adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    if (SkColorSpace::Equals(colorSpace, SkColorSpace::MakeRGB(SkNamedTransferFn::kSRGB, SkNamedGamut::kDisplayP3).get()))
        return adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3));
    if (SkColorSpace::Equals(colorSpace, SkColorSpace::MakeSRGBLinear().get()))
        return adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB));

    // The named spaces are the ones canvases have. Any other is described by its profile.
    skcms_TransferFunction transferFunction;
    skcms_Matrix3x3 toXYZD50;
    if (colorSpace->isNumericalTransferFn(&transferFunction) && colorSpace->toXYZD50(&toXYZD50)) {
        if (auto profile = SkWriteICCProfile(transferFunction, toXYZD50)) {
            auto profileData = adoptCF(CFDataCreate(kCFAllocatorDefault, profile->bytes(), profile->size()));
            if (auto profileColorSpace = adoptCF(CGColorSpaceCreateWithICCData(profileData.get())))
                return profileColorSpace;
        }
    }
    return adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
}

static RetainPtr<CGImageRef> createCGImage(std::span<const uint8_t> pixels, const IntSize& size, size_t bytesPerRow, CGBitmapInfo bitmapInfo, SkColorSpace* colorSpace)
{
    // The pixels outlive the image: it is encoded before its caller returns.
    auto dataProvider = adoptCF(CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size(), nullptr));
    if (!dataProvider)
        return nullptr;
    return adoptCF(CGImageCreate(size.width(), size.height(), 8, 32, bytesPerRow, cgColorSpace(colorSpace).get(), bitmapInfo, dataProvider.get(), nullptr, false, kCGRenderingIntentDefault));
}

static Vector<uint8_t> encode(CGImageRef image, const String& mimeType, std::optional<double> quality)
{
    if (!image)
        return { };

    auto typeIdentifier = typeIdentifierForMIMEType(mimeType);
    if (!typeIdentifier)
        return { };

    Vector<uint8_t> result;
    CGDataConsumerCallbacks callbacks {
        [](void* info, const void* buffer, size_t count) -> size_t {
            static_cast<Vector<uint8_t>*>(info)->append(unsafeMakeSpan(static_cast<const uint8_t*>(buffer), count));
            return count;
        },
        nullptr
    };
    auto consumer = adoptCF(CGDataConsumerCreate(&result, &callbacks));
    auto destination = adoptCF(CGImageDestinationCreateWithDataConsumer(consumer.get(), typeIdentifier, 1, nullptr));
    if (!destination)
        return { };

    RetainPtr<CFDictionaryRef> properties;
    if (CFEqual(typeIdentifier, CFSTR("public.jpeg")) && quality && *quality >= 0.0 && *quality <= 1.0) {
        auto compressionQuality = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &*quality));
        const void* key = kCGImageDestinationLossyCompressionQuality;
        const void* value = compressionQuality.get();
        properties = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    }
    CGImageDestinationAddImage(destination.get(), image, properties.get());
    if (!CGImageDestinationFinalize(destination.get()))
        return { };
    return result;
}

Vector<uint8_t> platformEncodeData(const NativeImage& nativeImage, const String& mimeType, std::optional<double> quality)
{
    auto image = nativeImage.platformImage();
    if (!image)
        return { };

    // An image buffer's image is premultiplied BGRA for ImageIO in Safari, opaque or not, in the rows of
    // an IOSurface: the row length decides how ImageIO cuts a TIFF into strips.
    auto alphaType = image->alphaType() == kUnpremul_SkAlphaType ? kUnpremul_SkAlphaType : kPremul_SkAlphaType;
    CGBitmapInfo bitmapInfo = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | static_cast<CGBitmapInfo>(alphaType == kUnpremul_SkAlphaType ? kCGImageAlphaFirst : kCGImageAlphaPremultipliedFirst);
    IntSize size { image->width(), image->height() };
    auto imageInfo = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, alphaType, image->refColorSpace());
    size_t bytesPerRow = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, imageInfo.minRowBytes());
    auto byteSize = CheckedSize { bytesPerRow } * static_cast<size_t>(size.height());
    if (byteSize.hasOverflowed())
        return { };
    auto pixels = MallocSpan<uint8_t>::tryZeroedMalloc(byteSize.value());
    if (!pixels)
        return { };

    if (image->isTextureBacked() && !PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent())
        return { };
    if (!image->readPixels(nativeImage.grContext(), imageInfo, pixels.mutableSpan().data(), bytesPerRow, 0, 0))
        return { };
    return encode(createCGImage(pixels.span(), size, bytesPerRow, bitmapInfo, image->colorSpace()).get(), mimeType, quality);
}

Vector<uint8_t> encodeData(const PixelBuffer& source, const String& mimeType, std::optional<double> quality)
{
    ASSERT(MIMETypeRegistry::isSupportedImageMIMETypeForEncoding(mimeType));

    auto data = source.bytes();
    CGImageAlphaInfo alphaInfo = kCGImageAlphaLast;
    Vector<uint8_t> premultipliedData;
    if (MIMETypeRegistry::isJPEGMIMEType(mimeType)) {
        // JPEG has no alpha channel: the pixels are composited on black, with the arithmetic of the Cocoa ports.
        if (!premultipliedData.tryGrow(data.size()))
            return { };
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            unsigned alpha = data[i + 3];
            if (alpha != 255) {
                premultipliedData[i] = data[i] * alpha / 255;
                premultipliedData[i + 1] = data[i + 1] * alpha / 255;
                premultipliedData[i + 2] = data[i + 2] * alpha / 255;
            } else {
                premultipliedData[i] = data[i];
                premultipliedData[i + 1] = data[i + 1];
                premultipliedData[i + 2] = data[i + 2];
            }
        }
        alphaInfo = kCGImageAlphaNoneSkipLast;
        data = premultipliedData.mutableSpan();
    }

    auto size = source.size();
    CGBitmapInfo bitmapInfo = static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault) | static_cast<CGBitmapInfo>(alphaInfo);
    return encode(createCGImage(data, size, 4 * size.width(), bitmapInfo, source.format().colorSpace.platformColorSpace().get()).get(), mimeType, quality);
}

String encodeDataURL(const PixelBuffer& source, const String& mimeType, std::optional<double> quality)
{
    auto encodedData = encodeData(source, mimeType, quality);
    if (encodedData.isEmpty())
        return "data:,"_s;
    return makeString("data:"_s, mimeType, ";base64,"_s, base64Encoded(encodedData));
}

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
