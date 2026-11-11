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
#include "WebGPUCompositorIntegrationImpl.h"

#if HAVE(WEBGPU_IMPLEMENTATION) && !HAVE(IOSURFACE)

#include "NativeImage.h"
#include "WebGPUIOSurfaceImageSkia.h"
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <array>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkImageInfo.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

// The render buffers of WebGPUCompositorIntegrationImpl for a port on Darwin without
// WebCore::IOSurface, which is CoreGraphics': the surfaces are made here and read with Skia.

namespace WebCore {

static RetainPtr<IOSurfaceRef> createRenderBuffer(int width, int height, uint32_t pixelFormat)
{
    constexpr int bytesPerElement = 4;
    int bytesPerRow = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * bytesPerElement);
    int allocSize = IOSurfaceAlignProperty(kIOSurfaceAllocSize, height * bytesPerRow);
    int signedPixelFormat = pixelFormat;
    auto number = [](const int& value) {
        return adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value));
    };
    std::array numbers { number(width), number(height), number(signedPixelFormat), number(bytesPerElement), number(bytesPerRow), number(allocSize) };
    std::array<const void*, 7> keys { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfacePixelFormat, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow, kIOSurfaceAllocSize, kIOSurfaceName };
    std::array<const void*, 7> values { numbers[0].get(), numbers[1].get(), numbers[2].get(), numbers[3].get(), numbers[4].get(), numbers[5].get(), CFSTR("WebKit WebGPU") };
    auto properties = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, keys.data(), values.data(), keys.size(), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    return adoptCF(IOSurfaceCreate(properties.get()));
}

Vector<MachSendRight> WebGPUCompositorIntegrationImpl::recreateRenderBuffers(int width, int height, WebCore::ColorSpace&& colorSpace, WebCore::AlphaPremultiplication alphaMode, WebGPU::TextureFormat textureFormat, unsigned bufferCount, WebGPU::Device& device)
{
    m_renderBuffers.clear();
    m_device = device;
    m_alphaMode = alphaMode;
    m_colorSpace = WTF::move(colorSpace);

    if (RefPtr presentationContext = m_presentationContext)
        presentationContext->unconfigure();

    constexpr int max2DTextureSize = 16384;
    width = std::max(1, std::min(max2DTextureSize, width));
    height = std::max(1, std::min(max2DTextureSize, height));

    // The pixel formats WebCore::IOSurface gives the same texture formats on the Cocoa ports.
    bool isRGBA = textureFormat == WebGPU::TextureFormat::Rgba8unorm || textureFormat == WebGPU::TextureFormat::Rgba8unormSRGB;
    uint32_t pixelFormat = isRGBA ? 'RGBA' : 'BGRA';
    for (unsigned bufferIndex = 0; bufferIndex < std::min<unsigned>(3u, bufferCount); ++bufferIndex) {
        if (auto buffer = createRenderBuffer(width, height, pixelFormat))
            m_renderBuffers.append(WTF::move(buffer));
    }

    auto renderBuffers = m_renderBuffers.map([](auto& renderBuffer) {
        return renderBuffer.get();
    });
    m_renderBuffersWereRecreatedCallback(renderBuffers.span());

    return m_renderBuffers.map([](const auto& renderBuffer) {
        return MachSendRight::adopt(IOSurfaceCreateMachPort(renderBuffer.get()));
    });
}

// PresentationContextIOSurface::getTextureAsNativeImage makes the image of the texture's own bytes,
// which are RGBA for the 8-bit format it reads back this way.
static sk_sp<SkImage> createImageFromTextureContents(CGImageRef textureContents, bool isOpaque, sk_sp<SkColorSpace>&& colorSpace)
{
    if (CGImageGetBitsPerComponent(textureContents) != 8 || CGImageGetBitsPerPixel(textureContents) != 32)
        return nullptr;
    auto bytes = adoptCF(CGDataProviderCopyData(CGImageGetDataProvider(textureContents)));
    if (!bytes)
        return nullptr;
    auto info = SkImageInfo::Make(CGImageGetWidth(textureContents), CGImageGetHeight(textureContents), isOpaque ? kRGB_888x_SkColorType : kRGBA_8888_SkColorType, isOpaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType, WTF::move(colorSpace));
    size_t bytesPerRow = CGImageGetBytesPerRow(textureContents);
    size_t length = CFDataGetLength(bytes.get());
    if (bytesPerRow < info.minRowBytes() || length < info.computeByteSize(bytesPerRow))
        return nullptr;
    return SkImages::RasterFromData(info, SkData::MakeWithCopy(CFDataGetBytePtr(bytes.get()), length), bytesPerRow);
}

void WebGPUCompositorIntegrationImpl::withDisplayBufferAsNativeImage(uint32_t bufferIndex, Function<void(WebCore::NativeImage*)> completion)
{
    if (bufferIndex >= m_renderBuffers.size() || !m_device.get() || !m_colorSpace)
        return completion(nullptr);

    // The canvas makes its buffers without an alpha channel unless it is premultiplied.
    bool isOpaque = m_alphaMode != WebCore::AlphaPremultiplication::Premultiplied;

    // This waits for the commands that draw the frame. A texture whose format is not the surface's
    // comes back as an image of its own; the surface holds the frame of any other.
    RetainPtr<CGImageRef> textureContents;
    bool isIOSurfaceSupportedFormat = false;
    if (RefPtr presentationContext = m_presentationContext)
        textureContents = presentationContext->getTextureAsNativeImage(bufferIndex, isIOSurfaceSupportedFormat);

    sk_sp<SkImage> image;
    if (textureContents)
        image = createImageFromTextureContents(textureContents.get(), isOpaque, m_colorSpace->platformColorSpace());
    else if (isIOSurfaceSupportedFormat)
        image = createRasterImageFromIOSurface(m_renderBuffers[bufferIndex].get(), isOpaque, m_colorSpace->platformColorSpace());

    RefPtr nativeImage = image ? NativeImage::create(WTF::move(image)) : nullptr;
    completion(nativeImage.get());
}

} // namespace WebCore

#endif // HAVE(WEBGPU_IMPLEMENTATION) && !HAVE(IOSURFACE)
