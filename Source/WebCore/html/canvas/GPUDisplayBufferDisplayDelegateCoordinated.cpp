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
#include "GPUDisplayBufferDisplayDelegateCoordinated.h"

#if HAVE(WEBGPU_IMPLEMENTATION) && USE(COORDINATED_GRAPHICS) && USE(SKIA)

#include "CoordinatedPlatformLayer.h"
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#include "WebGPUIOSurfaceImageSkia.h"
#include <IOSurface/IOSurfaceRef.h>

namespace WebCore {

GPUDisplayBufferDisplayDelegate::GPUDisplayBufferDisplayDelegate()
    : m_colorSpace(ColorSpace::SRGB())
{
}

GPUDisplayBufferDisplayDelegate::~GPUDisplayBufferDisplayDelegate() = default;

void GPUDisplayBufferDisplayDelegate::setDisplayBuffer(WTF::MachSendRight& displayBuffer)
{
    m_layerBuffer = nullptr;
    m_surface = displayBuffer ? adoptCF(IOSurfaceLookupFromMachPort(displayBuffer.sendRight())) : nullptr;
}

void GPUDisplayBufferDisplayDelegate::setDisplayBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&& displayBuffer)
{
    m_layerBuffer = WTF::move(displayBuffer);
    m_surface = nullptr;
}

std::unique_ptr<CoordinatedPlatformLayerBuffer> GPUDisplayBufferDisplayDelegate::createLayerBuffer() const
{
    if (!m_surface)
        return nullptr;

    auto alphaMode = m_isOpaque ? CoordinatedPlatformLayerBuffer::AlphaMode::Opaque : CoordinatedPlatformLayerBuffer::AlphaMode::Premultiplied;
    auto image = createTextureImageFromIOSurface(m_surface.get(), threadSafeGrContext(), m_isOpaque, m_colorSpace.platformColorSpace());
    // A compositor that does not draw with the GPU gets the pixels.
    if (!image)
        image = createRasterImageFromIOSurface(m_surface.get(), m_isOpaque, m_colorSpace.platformColorSpace());
    if (!image)
        return nullptr;
    return makeUnique<CoordinatedPlatformLayerBufferSkiaImage>(WTF::move(image), alphaMode, CoordinatedPlatformLayerBuffer::Rotation::None);
}

void GPUDisplayBufferDisplayDelegate::display(CoordinatedPlatformLayer& layer, std::optional<Damage>&& damage)
{
    assertIsHeld(layer.lock());
    // The surface is one of the few the canvas cycles through, so each display gets a buffer of
    // its own: the compositor takes a new buffer as new contents.
    auto buffer = m_layerBuffer ? std::exchange(m_layerBuffer, nullptr) : createLayerBuffer();
    if (buffer)
        layer.setContentsBuffer(WTF::move(buffer), WTF::move(damage));
}

} // namespace WebCore

#endif // HAVE(WEBGPU_IMPLEMENTATION) && USE(COORDINATED_GRAPHICS) && USE(SKIA)
