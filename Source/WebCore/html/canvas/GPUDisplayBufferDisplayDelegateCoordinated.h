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

#if HAVE(WEBGPU_IMPLEMENTATION) && USE(COORDINATED_GRAPHICS) && USE(SKIA)

#include "ColorSpace.h"
#include "ContentsFormat.h"
#include "GraphicsLayerContentsDisplayDelegate.h"
#include <wtf/MachSendRight.h>
#include <wtf/Ref.h>
#include <wtf/RetainPtr.h>

typedef struct __IOSurface* IOSurfaceRef;

namespace WebCore {

// Makes the render buffer that a WebGPU canvas last presented the contents of the canvas's layer.
// The compositor textures from the buffer's IOSurface, so a frame is shown without a copy.
// Its interface to GPUCanvasContextCocoa is that of the delegate of the Cocoa ports.
class GPUDisplayBufferDisplayDelegate final : public GraphicsLayerContentsDisplayDelegate {
public:
    static Ref<GPUDisplayBufferDisplayDelegate> create()
    {
        return adoptRef(*new GPUDisplayBufferDisplayDelegate);
    }
    ~GPUDisplayBufferDisplayDelegate();

    void setDisplayBuffer(WTF::MachSendRight&);
    void setColorSpace(ColorSpace&& colorSpace) { m_colorSpace = WTF::move(colorSpace); }
    void setContentsFormat(ContentsFormat contentsFormat) { m_contentsFormat = contentsFormat; }
    ContentsFormat contentsFormat() const { return m_contentsFormat; }
    bool isOpaque() const { return m_isOpaque; }
    void setOpaque(bool opaque) { m_isOpaque = opaque; }
    bool hasExtendedRange() const { return false; }

private:
    GPUDisplayBufferDisplayDelegate();

    // GraphicsLayerContentsDisplayDelegate overrides.
    void setDisplayBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&&) final;
    void display(CoordinatedPlatformLayer&, std::optional<Damage>&&) final;

    std::unique_ptr<CoordinatedPlatformLayerBuffer> createLayerBuffer() const;

    RetainPtr<IOSurfaceRef> m_surface;
    // A buffer given ready-made, which is shown in place of the surface.
    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_layerBuffer;
    ColorSpace m_colorSpace;
    ContentsFormat m_contentsFormat { ContentsFormat::RGBA8 };
    bool m_isOpaque { true };
};

} // namespace WebCore

#endif // HAVE(WEBGPU_IMPLEMENTATION) && USE(COORDINATED_GRAPHICS) && USE(SKIA)
