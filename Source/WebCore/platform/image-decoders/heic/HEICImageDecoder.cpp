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
#include "HEICImageDecoder.h"

#if USE(SKIA) && OS(DARWIN)

#include "SharedBuffer.h"
#include <Accelerate/Accelerate.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <wtf/RetainPtr.h>

namespace WebCore {

HEICImageDecoder::HEICImageDecoder(AlphaOption alphaOption, GammaAndColorProfileOption gammaAndColorProfileOption)
    : ScalableImageDecoder(alphaOption, gammaAndColorProfileOption)
{
}

// The source reads `data` in place, so it is only used while its caller holds the buffer.
static RetainPtr<CGImageSourceRef> createImageSource(const SharedBuffer& data)
{
    auto bytes = data.span();
    auto sourceData = adoptCF(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, bytes.data(), bytes.size(), kCFAllocatorNull));
    if (!sourceData)
        return nullptr;
    auto source = adoptCF(CGImageSourceCreateWithData(sourceData.get(), nullptr));
    if (!source)
        return nullptr;

    // ImageIO decodes every type it knows: only the ones this decoder is chosen for are let through.
    auto type = CGImageSourceGetType(source.get());
    if (!type)
        return nullptr;
    if (!CFEqual(type, CFSTR("public.heic")) && !CFEqual(type, CFSTR("public.heif")) && !CFEqual(type, CFSTR("public.heics")) && !CFEqual(type, CFSTR("public.heifs")))
        return nullptr;
    return source;
}

static bool integerProperty(CFDictionaryRef properties, CFStringRef key, int& value)
{
    auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, key));
    return number && CFGetTypeID(number) == CFNumberGetTypeID() && CFNumberGetValue(number, kCFNumberIntType, &value);
}

void HEICImageDecoder::tryDecodeSize(bool allDataReceived)
{
    // ImageIO knows the size once the boxes describing the picture have arrived, wherever the file has
    // them. Stills are small: the size is taken when the file is complete.
    if (!allDataReceived)
        return;

    auto source = createImageSource(*m_data);
    if (!source || !CGImageSourceGetCount(source.get())) {
        setFailed();
        return;
    }
    auto properties = adoptCF(CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr));
    int width = 0;
    int height = 0;
    if (!properties || !integerProperty(properties.get(), kCGImagePropertyPixelWidth, width) || !integerProperty(properties.get(), kCGImagePropertyPixelHeight, height) || width <= 0 || height <= 0) {
        setFailed();
        return;
    }
    if (!setSize(IntSize(width, height)))
        return;

    // The size and the pixels are the stored picture's: its rotation and mirroring are applied when it is drawn.
    int orientation = 1;
    if (integerProperty(properties.get(), kCGImagePropertyOrientation, orientation))
        m_orientation = ImageOrientation::fromEXIFValue(orientation);
    m_frameCount = 1;
}

ScalableImageDecoderFrame* HEICImageDecoder::frameBufferAtIndex(size_t index)
{
    assertIsHeld(m_lock);
    if (index >= m_frameCount)
        return nullptr;

    if (m_frameBufferCache.size() != m_frameCount)
        m_frameBufferCache.resize(m_frameCount);
    if (!m_frameBufferCache[index].isComplete())
        decode();
    return &m_frameBufferCache[index];
}

void HEICImageDecoder::decode()
{
    assertIsHeld(m_lock);
    if (failed())
        return;

    auto source = createImageSource(*m_data);
    RetainPtr<CGImageRef> image;
    if (source)
        image = adoptCF(CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr));
    auto imageSize = size();
    if (!image || static_cast<size_t>(imageSize.width()) != CGImageGetWidth(image.get()) || static_cast<size_t>(imageSize.height()) != CGImageGetHeight(image.get())) {
        setFailed();
        return;
    }

    auto& buffer = m_frameBufferCache[0];
    if (buffer.isInvalid() && !buffer.initialize(imageSize, premultiplyAlpha())) {
        setFailed();
        return;
    }
    buffer.setDecodingStatus(DecodingStatus::Partial);

    // A frame's pixels are premultiplied BGRA, which a bitmap context can draw into where they are.
    // The picture is converted to sRGB, unless its own colors are asked for and can be stored as they are.
    auto colorSpace = adoptCF(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    if (m_ignoreGammaAndColorProfile) {
        RetainPtr imageColorSpace = CGImageGetColorSpace(image.get());
        if (imageColorSpace && CGColorSpaceGetModel(imageColorSpace.get()) == kCGColorSpaceModelRGB && CGColorSpaceSupportsOutput(imageColorSpace.get()))
            colorSpace = WTF::move(imageColorSpace);
    }
    auto pixels = buffer.backingStore()->pixelsStartingAt(0, 0);
    size_t bytesPerRow = static_cast<size_t>(imageSize.width()) * sizeof(uint32_t);
    auto context = adoptCF(CGBitmapContextCreate(pixels.data(), imageSize.width(), imageSize.height(), 8, bytesPerRow, colorSpace.get(), static_cast<uint32_t>(kCGBitmapByteOrder32Little) | static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst)));
    if (!context) {
        setFailed();
        return;
    }
    CGContextSetBlendMode(context.get(), kCGBlendModeCopy);
    CGContextDrawImage(context.get(), CGRectMake(0, 0, imageSize.width(), imageSize.height()), image.get());

    auto alphaInfo = CGImageGetAlphaInfo(image.get());
    bool hasAlpha = alphaInfo != kCGImageAlphaNone && alphaInfo != kCGImageAlphaNoneSkipFirst && alphaInfo != kCGImageAlphaNoneSkipLast;
    if (hasAlpha && !premultiplyAlpha()) {
        // The alpha is the last byte of a pixel in this order of bytes too.
        vImage_Buffer pixelBuffer { pixels.data(), static_cast<vImagePixelCount>(imageSize.height()), static_cast<vImagePixelCount>(imageSize.width()), bytesPerRow };
        vImageUnpremultiplyData_RGBA8888(&pixelBuffer, &pixelBuffer, kvImageNoFlags);
    }
    buffer.setHasAlpha(hasAlpha);
    buffer.setDecodingStatus(DecodingStatus::Complete);
}

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
