/*
 * Copyright (C) 2021-2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if HAVE(WEBGPU_IMPLEMENTATION)

#include "WebGPUCompositorIntegration.h"
#include "WebGPUDevice.h"

#if HAVE(IOSURFACE)
#include <WebCore/IOSurface.h>
#else
#include <WebCore/ColorSpace.h>
#endif
#include <WebCore/WebGPUCppAPI.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

#if OS(DARWIN)
#include <wtf/MachSendRight.h>
#include <wtf/RetainPtr.h>
#include <wtf/spi/cocoa/IOSurfaceSPI.h>
#endif

namespace WebCore {
class NativeImage;
class WebGPUWorkQueue;
}

namespace WebCore {


class WebGPUCompositorIntegrationImpl final : public WebGPUCompositorIntegration {
    WTF_MAKE_TZONE_ALLOCATED(WebGPUCompositorIntegrationImpl);
public:
    // The work queue is that of an instance used in the process of its caller.
    static Ref<WebGPUCompositorIntegrationImpl> create(WebGPUWorkQueue* workQueue = nullptr)
    {
        return adoptRef(*new WebGPUCompositorIntegrationImpl(workQueue));
    }

    virtual ~WebGPUCompositorIntegrationImpl();

    void setPresentationContext(::WebGPU::PresentationContext& presentationContext)
    {
        lazyInitialize(m_presentationContext, Ref { presentationContext });
    }

    void registerCallbacks(WTF::Function<void(std::span<const IOSurfaceRef>)>&& renderBuffersWereRecreatedCallback, WTF::Function<void(CompletionHandler<void()>&&)>&& onSubmittedWorkScheduledCallback)
    {
        ASSERT(!m_renderBuffersWereRecreatedCallback);
        m_renderBuffersWereRecreatedCallback = WTF::move(renderBuffersWereRecreatedCallback);
        ASSERT(!m_onSubmittedWorkScheduledCallback);
        m_onSubmittedWorkScheduledCallback = WTF::move(onSubmittedWorkScheduledCallback);
    }

    void withDisplayBufferAsNativeImage(uint32_t bufferIndex, Function<void(WebCore::NativeImage*)>) final;
    void paintCompositedResultsToCanvas(WebCore::ImageBuffer&, uint32_t) final;

private:

    explicit WebGPUCompositorIntegrationImpl(WebGPUWorkQueue*);

    WebGPUCompositorIntegrationImpl(const WebGPUCompositorIntegrationImpl&) = delete;
    WebGPUCompositorIntegrationImpl(WebGPUCompositorIntegrationImpl&&) = delete;
    WebGPUCompositorIntegrationImpl& operator=(const WebGPUCompositorIntegrationImpl&) = delete;
    WebGPUCompositorIntegrationImpl& operator=(WebGPUCompositorIntegrationImpl&&) = delete;

    bool isWebGPUCompositorIntegrationImpl() const final { return true; }

    void prepareForDisplay(uint32_t frameIndex, CompletionHandler<void()>&&) override;
    void updateContentsHeadroom(float) override;

    Seconds lastFrameGPUCost() const override;
    Seconds lastFramePresentStall() const override;

#if OS(DARWIN)
    Vector<MachSendRight> recreateRenderBuffers(int width, int height, WebCore::ColorSpace&&, WebCore::AlphaPremultiplication, ::WebGPU::TextureFormat, unsigned bufferCount, ::WebGPU::Device&) override;

#if HAVE(IOSURFACE)
    Vector<UniqueRef<WebCore::IOSurface>> m_renderBuffers;
#else
    // Without WebCore::IOSurface, which draws with CoreGraphics: the surfaces themselves.
    Vector<RetainPtr<IOSurfaceRef>> m_renderBuffers;
    std::optional<WebCore::ColorSpace> m_colorSpace;
#endif
    WebCore::AlphaPremultiplication m_alphaMode { WebCore::AlphaPremultiplication::Premultiplied };
    WTF::Function<void(std::span<const IOSurfaceRef>)> m_renderBuffersWereRecreatedCallback;
#endif

    WTF::Function<void(CompletionHandler<void()>&&)> m_onSubmittedWorkScheduledCallback;

    const RefPtr<WebGPUWorkQueue> m_workQueue;
    const RefPtr<::WebGPU::PresentationContext> m_presentationContext;
    ThreadSafeWeakPtr<::WebGPU::Device> m_device;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::WebGPUCompositorIntegrationImpl)
    static bool isType(const WebCore::WebGPUCompositorIntegration& compositorIntegration) { return compositorIntegration.isWebGPUCompositorIntegrationImpl(); }
SPECIALIZE_TYPE_TRAITS_END()

#endif // HAVE(WEBGPU_IMPLEMENTATION)
