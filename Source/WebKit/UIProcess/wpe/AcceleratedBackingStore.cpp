/*
 * Copyright (C) 2023 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AcceleratedBackingStore.h"

#if ENABLE(WPE_PLATFORM)
#include "AcceleratedBackingStoreMessages.h"
#include "AcceleratedSurfaceMessages.h"
#include "ViewSnapshotStore.h"
#include "WebPageProxy.h"
#include "WebProcessProxy.h"
#include <WebCore/ShareableBitmap.h>
#include <wpe/wpe-platform.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/GLibExtras.h>
#include <wtf/glib/GSpanExtras.h>
#include <wtf/glib/GUniquePtr.h>

#if OS(DARWIN)
#include "IOSurfaceChannel.h"
#endif

#if OS(WINDOWS)
#include <cmath>
#include <windows.h>
#endif

#if OS(ANDROID)
#include <drm/drm_fourcc.h>
#elif USE(LIBDRM)
#include <drm_fourcc.h>
#else
// The two formats named here, where the system has no libdrm headers.
#define DRM_FORMAT_XRGB8888 0x34325258
#define DRM_FORMAT_ARGB8888 0x34325241
#endif

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkStream.h>
#include <skia/encode/SkPngEncoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedBackingStore);

Ref<AcceleratedBackingStore> AcceleratedBackingStore::create(WebPageProxy& webPage, WPEView* view)
{
    return adoptRef(*new AcceleratedBackingStore(webPage, view));
}

AcceleratedBackingStore::AcceleratedBackingStore(WebPageProxy& webPage, WPEView* view)
    : m_webPage(webPage)
    , m_wpeView(view)
    , m_fenceMonitor([this] {
        if (m_webPage && m_pendingBuffer)
            renderPendingBuffer();
    })
    , m_legacyMainFrameProcess(webPage.legacyMainFrameProcess())
{
    g_signal_connect(m_wpeView.get(), "buffer-rendered", G_CALLBACK(+[](WPEView*, WPEBuffer*, gpointer userData) {
        auto& backingStore = *static_cast<AcceleratedBackingStore*>(userData);
        backingStore.bufferRendered();
    }), this);
    g_signal_connect(m_wpeView.get(), "buffer-released", G_CALLBACK(+[](WPEView*, WPEBuffer* buffer, gpointer userData) {
        auto& backingStore = *static_cast<AcceleratedBackingStore*>(userData);
        backingStore.bufferReleased(buffer);
    }), this);
    g_signal_connect(m_wpeView.get(), "notify::toplevel", G_CALLBACK(+[](WPEView* view, GParamSpec*, gpointer userData) {
        auto& backingStore = *static_cast<AcceleratedBackingStore*>(userData);
        backingStore.toplevelChanged();
    }), this);
#if OS(WINDOWS)
    g_signal_connect(m_wpeView.get(), "notify::native-window", G_CALLBACK(+[](WPEView*, GParamSpec*, gpointer userData) {
        auto& backingStore = *static_cast<AcceleratedBackingStore*>(userData);
        backingStore.sendNativeWindow();
    }), this);
    g_signal_connect(m_wpeView.get(), "resized", G_CALLBACK(+[](WPEView*, gpointer userData) {
        auto& backingStore = *static_cast<AcceleratedBackingStore*>(userData);
        backingStore.sizePresentationWindow();
    }), this);
    g_signal_connect(m_wpeView.get(), "notify::scale", G_CALLBACK(+[](WPEView*, GParamSpec*, gpointer userData) {
        auto& backingStore = *static_cast<AcceleratedBackingStore*>(userData);
        backingStore.sizePresentationWindow();
    }), this);
#endif
}

AcceleratedBackingStore::~AcceleratedBackingStore()
{
    if (m_surfaceID) {
        if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
            legacyMainFrameProcess->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
    }
    g_signal_handlers_disconnect_by_data(m_wpeView.get(), this);
#if OS(WINDOWS)
    if (m_presentationWindow)
        ::DestroyWindow(static_cast<HWND>(m_presentationWindow));
#endif
}

void AcceleratedBackingStore::updateSurfaceID(uint64_t surfaceID)
{
    if (m_surfaceID == surfaceID)
        return;

    if (m_surfaceID) {
        if (m_pendingBuffer) {
            frameDone();
            m_pendingBuffer = nullptr;
            m_pendingDamageRects = { };
        }
        m_buffers.clear();
        m_bufferIDs.clear();
        if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
            legacyMainFrameProcess->removeMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID);
    }

    m_surfaceID = surfaceID;
    if (m_surfaceID && m_webPage) {
        m_legacyMainFrameProcess = m_webPage->legacyMainFrameProcess();
        protect(*m_legacyMainFrameProcess)->addMessageReceiver(Messages::AcceleratedBackingStore::messageReceiverName(), m_surfaceID, *this);
#if OS(WINDOWS)
        // A new surface (a new page, or a new web process) presents into the
        // view's window as the last one did.
        if (wpe_view_get_native_window(m_wpeView.get()))
            sendNativeWindow();
#endif
    }
}

#if OS(WINDOWS)
// The presentation windows are this thread's (the embedder's), so that it
// never waits for a web process: they paint nothing (the swap chain of the
// web process draws them) and leave all input to the window beneath.
static LRESULT CALLBACK presentationWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    default:
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

static HWND createPresentationWindow(HWND parent, int width, int height)
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        WNDCLASSW windowClass { };
        windowClass.lpfnWndProc = presentationWindowProcedure;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"WPEPresentationWindow";
        ::RegisterClassW(&windowClass);
    });
    auto window = ::CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"WPEPresentationWindow", nullptr, WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
        0, 0, width, height, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (window)
        ::SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return window;
}

void AcceleratedBackingStore::sizePresentationWindow()
{
    if (!m_presentationWindow)
        return;

    auto scale = wpe_view_get_scale(m_wpeView.get());
    int width = std::max(1, static_cast<int>(std::ceil(wpe_view_get_width(m_wpeView.get()) * scale)));
    int height = std::max(1, static_cast<int>(std::ceil(wpe_view_get_height(m_wpeView.get()) * scale)));
    ::SetWindowPos(static_cast<HWND>(m_presentationWindow), nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void AcceleratedBackingStore::sendNativeWindow()
{
    if (!m_surfaceID)
        return;

    // The web process presents its frames into a window of the view's
    // window, or with none sends them as buffers again; either way from its
    // next frame, which the change schedules. Without one, the presentation
    // window stays, showing the last frame.
    RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get();
    if (!legacyMainFrameProcess)
        return;

    uintptr_t window = 0;
    if (auto* parent = wpe_view_get_native_window(m_wpeView.get())) {
        auto processID = legacyMainFrameProcess->processID();
        if (!m_presentationWindow || !::IsWindow(static_cast<HWND>(m_presentationWindow)) || m_presentationParent != parent
            || m_presentationSurfaceID != m_surfaceID || m_presentationProcessID != processID) {
            auto* previous = static_cast<HWND>(m_presentationWindow);
            m_presentationWindow = createPresentationWindow(static_cast<HWND>(parent), 1, 1);
            m_presentationParent = m_presentationWindow ? parent : nullptr;
            m_presentationSurfaceID = m_presentationWindow ? m_surfaceID : 0;
            m_presentationProcessID = m_presentationWindow ? processID : 0;
            sizePresentationWindow();
            if (previous)
                ::DestroyWindow(previous);
        }
        window = reinterpret_cast<uintptr_t>(m_presentationWindow);
    }
    legacyMainFrameProcess->send(Messages::AcceleratedSurface::SetNativeWindow(window), m_surfaceID);
}
#endif

void AcceleratedBackingStore::didChangeBufferConfiguration(uint32_t bufferCount)
{
    m_targetBufferCount = bufferCount;
}

void AcceleratedBackingStore::notifyBufferConfigurationIfNeeded()
{
    if (!m_targetBufferCount || *m_targetBufferCount != m_buffers.size())
        return;

    m_targetBufferCount.reset();

    Vector<WPEBuffer*, 2> buffers(ReserveCapacityTag { }, m_buffers.size());
    for (auto& value : m_buffers.values())
        buffers.append(value.get());

    auto buffersSpan = buffers.mutableSpan();
    wpe_view_buffers_changed(m_wpeView.get(), buffersSpan.data(), buffersSpan.size());
}

void AcceleratedBackingStore::didCreateDMABufBuffer(RendererBufferIdentifier id, WebCore::DMABufBufferAttributes&& dmaBufAttributes, RendererBufferFormat::Usage usage)
{
#if OS(WINDOWS)
    // WPE Platform has no DMA-BUF buffers there, and no web process a
    // DMA-BUF to offer: frames are shared memory.
    UNUSED_PARAM(id);
    UNUSED_PARAM(dmaBufAttributes);
    UNUSED_PARAM(usage);
#else
    Vector<int> fileDescriptors(ReserveCapacityTag { }, dmaBufAttributes.fds.size());
    for (auto& fd : dmaBufAttributes.fds)
        fileDescriptors.append(fd.release());
    GRefPtr<WPEBuffer> buffer = adoptGRef(WPE_BUFFER(wpe_buffer_dma_buf_new(wpe_view_get_display(m_wpeView.get()), dmaBufAttributes.size.width(), dmaBufAttributes.size.height(), dmaBufAttributes.fourcc.value, dmaBufAttributes.fds.size(), fileDescriptors.mutableSpan().data(), dmaBufAttributes.offsets.mutableSpan().data(), dmaBufAttributes.strides.mutableSpan().data(), dmaBufAttributes.modifier)));
    g_object_set_data(G_OBJECT(buffer.get()), "wk-buffer-format-usage", GUINT_TO_POINTER(usage));
    m_bufferIDs.add(buffer.get(), id);
    m_buffers.add(id, WTF::move(buffer));

    notifyBufferConfigurationIfNeeded();
#endif
}

void AcceleratedBackingStore::didCreateSHMBuffer(RendererBufferIdentifier id, WebCore::ShareableBitmap::Handle&& handle)
{
    auto bitmap = WebCore::ShareableBitmap::create(WTF::move(handle), WebCore::SharedMemory::Protection::ReadOnly);
    if (!bitmap)
        return;

    auto size = bitmap->size();
    auto data = bitmap->span();
    auto stride = bitmap->bytesPerRow();
    GRefPtr<GBytes> bytes = adoptGRef(g_bytes_new_with_free_func(data.data(), data.size(), [](gpointer userData) {
        adoptRef(static_cast<WebCore::ShareableBitmap*>(userData));
    }, bitmap.leakRef()));

    GRefPtr<WPEBuffer> buffer = adoptGRef(WPE_BUFFER(wpe_buffer_shm_new(wpe_view_get_display(m_wpeView.get()), size.width(), size.height(), WPE_PIXEL_FORMAT_ARGB8888, bytes.get(), stride)));
    m_bufferIDs.add(buffer.get(), id);
    m_buffers.add(id, WTF::move(buffer));

    notifyBufferConfigurationIfNeeded();
}

#if OS(ANDROID)
void AcceleratedBackingStore::didCreateAndroidBuffer(RendererBufferIdentifier id, RefPtr<AHardwareBuffer>&& hardwareBuffer)
{
    RELEASE_ASSERT(hardwareBuffer);

    auto buffer = adoptGRef(WPE_BUFFER(wpe_buffer_android_new(wpe_view_get_display(m_wpeView.get()), hardwareBuffer.get())));
    m_bufferIDs.add(buffer.get(), id);
    m_buffers.add(id, WTF::move(buffer));

    notifyBufferConfigurationIfNeeded();
}
#endif // OS(ANDROID)

#if OS(DARWIN)
void AcceleratedBackingStore::didCreateIOSurfaceBuffer(RendererBufferIdentifier id)
{
    // The web process sent the surface over the channel before this message.
    RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get();
    auto* surface = legacyMainFrameProcess ? IOSurfaceChannel::takeSurface(legacyMainFrameProcess->processID(), id.toUInt64()) : nullptr;
    if (!surface) {
        g_warning("The IOSurface of buffer %" PRIu64 " did not arrive", id.toUInt64());
        return;
    }

    GRefPtr<WPEBuffer> buffer = adoptGRef(WPE_BUFFER(wpe_buffer_iosurface_new(wpe_view_get_display(m_wpeView.get()), surface)));
    CFRelease(surface);
    m_bufferIDs.add(buffer.get(), id);
    m_buffers.add(id, WTF::move(buffer));

    notifyBufferConfigurationIfNeeded();
}
#endif // OS(DARWIN)

void AcceleratedBackingStore::didDestroyBuffer(RendererBufferIdentifier id)
{
    if (auto buffer = m_buffers.take(id))
        m_bufferIDs.remove(buffer.get());

    notifyBufferConfigurationIfNeeded();
}

void AcceleratedBackingStore::frame(RendererBufferIdentifier bufferID, Rects&& damageRects, WTF::UnixFileDescriptor&& renderingFenceFD)
{
    ASSERT(!m_pendingBuffer);
    auto* buffer = m_buffers.get(bufferID);
    if (!buffer) {
        frameDone();
        return;
    }

    m_pendingBuffer = buffer;
    m_pendingDamageRects = WTF::move(damageRects);
    if (wpe_display_use_explicit_sync(wpe_view_get_display(m_wpeView.get()))) {
        wpe_buffer_set_rendering_fence(m_pendingBuffer.get(), renderingFenceFD.release());
        renderPendingBuffer();
    } else
        m_fenceMonitor.addFileDescriptor(WTF::move(renderingFenceFD));
}

static std::expected<SkImageInfo, String> getImageInfoFromBuffer(const  GRefPtr<WPEBuffer>& buffer)
{
    auto width = wpe_buffer_get_width(buffer.get());
    auto height = wpe_buffer_get_height(buffer.get());

#if !OS(WINDOWS)
    if (WPE_IS_BUFFER_DMA_BUF(buffer.get())) {
        auto* dmaBuffer = WPE_BUFFER_DMA_BUF(buffer.get());
        SkAlphaType alphaType = kPremul_SkAlphaType;
        if (wpe_buffer_dma_buf_get_format(dmaBuffer) == DRM_FORMAT_XRGB8888)
            alphaType = kOpaque_SkAlphaType;
        return SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, alphaType, SkColorSpace::MakeSRGB());
    }
#endif

    if (WPE_IS_BUFFER_SHM(buffer.get())) {
        SkAlphaType alphaType = kPremul_SkAlphaType;
        return SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, alphaType, SkColorSpace::MakeSRGB());
    }
#if OS(DARWIN)
    if (WPE_IS_BUFFER_IOSURFACE(buffer.get()))
        return SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
#endif
    return makeUnexpected("Failed to extract snapshot pixel information"_s);
}

static std::expected<Ref<ViewSnapshot>, String> saveBufferSnapshot(const GRefPtr<WPEBuffer>& buffer, std::optional<WebCore::IntRect>&& clipRect)
{
    GUniqueOutPtr<GError> error;
    GBytes* pixels = wpe_buffer_import_to_pixels(buffer.get(), &error.outPtr());

    if (!pixels) {
        g_warning("Failed to read current WPEBuffer for snapshot: %s", error->message);
        return makeUnexpected("Failed to read current WPEBuffer for snapshot"_s);
    }

    GRefPtr bytes = gBytesNew(span(pixels));

    auto info = getImageInfoFromBuffer(buffer);
    if (!info)
        return makeUnexpected(info.error());

    SkPixmap pixmap(info.value(), g_bytes_get_data(bytes.get(), nullptr), info->minRowBytes());

    if (clipRect) {
        SkIRect clippedRect = SkIRect::MakeXYWH(clipRect->x(), clipRect->y(), clipRect->width(), clipRect->height());
        SkImageInfo clippedInfo = info->makeWH(clipRect->width(), clipRect->height());
        SkPixmap clippedPixmap(info.value(), nullptr, clippedInfo.minRowBytes());
        if (!pixmap.extractSubset(&clippedPixmap, clippedRect))
            return makeUnexpected("Failed to extract clipped snapshot"_s);
        pixmap = clippedPixmap;
    }

    auto image = SkImages::RasterFromPixmap(pixmap, [](const void*, void* context) {
        g_bytes_unref(static_cast<GBytes*>(context));
    }, bytes.leakRef());

    if (!image)
        return makeUnexpected("Failed to create snapshot image"_s);

    return { ViewSnapshot::create(WTF::move(image)) };
}

std::expected<Ref<ViewSnapshot>, String> AcceleratedBackingStore::takeSnapshot(std::optional<WebCore::IntRect>&& clipRect)
{
    if (!m_committedBuffer && !m_pendingBuffer) [[unlikely]]
        return makeUnexpected("No buffer to create snapshot from"_s);

    return saveBufferSnapshot(m_committedBuffer ? m_committedBuffer : m_pendingBuffer, WTF::move(clipRect));
}

void AcceleratedBackingStore::renderPendingBuffer()
{
    if (!wpe_view_get_toplevel(m_wpeView.get()))
        return;

    // Rely on the layout of IntRect matching that of WPERectangle
    // to pass directly a pointer below instead of using copies.
    static_assert(sizeof(WebCore::IntRect) == sizeof(WPERectangle));

    ASSERT(m_pendingDamageRects.size() <= std::numeric_limits<guint>::max());
    const auto* rects = !m_pendingDamageRects.isEmpty() ? reinterpret_cast<const WPERectangle*>(m_pendingDamageRects.span().data()) : nullptr;

    GUniqueOutPtr<GError> error;
    if (!wpe_view_render_buffer(m_wpeView.get(), m_pendingBuffer.get(), rects, m_pendingDamageRects.size(), &error.outPtr())) {
        g_warning("Failed to render frame: %s", error->message);
        frameDone();
        m_pendingBuffer = nullptr;
    } else
        m_committedBuffer = WTF::move(m_pendingBuffer);
    m_pendingDamageRects = { };
}

void AcceleratedBackingStore::frameDone()
{
    if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
        legacyMainFrameProcess->send(Messages::AcceleratedSurface::FrameDone(), m_surfaceID);
}

void AcceleratedBackingStore::bufferRendered()
{
    frameDone();
}

void AcceleratedBackingStore::bufferReleased(WPEBuffer* buffer)
{
    if (auto id = m_bufferIDs.getOptional(buffer)) {
        auto releaseFence = UnixFileDescriptor { wpe_buffer_take_release_fence(buffer), UnixFileDescriptor::Adopt };

        if (RefPtr legacyMainFrameProcess = m_legacyMainFrameProcess.get())
            legacyMainFrameProcess->send(Messages::AcceleratedSurface::ReleaseBuffer(*id, WTF::move(releaseFence)), m_surfaceID);
    }
}

void AcceleratedBackingStore::toplevelChanged()
{
    if (!wpe_view_get_toplevel(m_wpeView.get()))
        return;

    if (!m_pendingBuffer || m_fenceMonitor.hasFileDescriptor())
        return;

    renderPendingBuffer();
}

RendererBufferDescription AcceleratedBackingStore::bufferDescription() const
{
    RendererBufferDescription description;
    auto* buffer = m_committedBuffer ? m_committedBuffer.get() : m_pendingBuffer.get();
    if (!buffer)
        return description;

#if !OS(WINDOWS)
    if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
        auto* dmabuf = WPE_BUFFER_DMA_BUF(buffer);
        description.type = RendererBufferDescription::Type::DMABuf;
        description.fourcc = wpe_buffer_dma_buf_get_format(dmabuf);
        description.modifier = wpe_buffer_dma_buf_get_modifier(dmabuf);
        description.usage = static_cast<RendererBufferFormat::Usage>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(buffer), "wk-buffer-format-usage")));
    } else
#endif
    if (WPE_IS_BUFFER_SHM(buffer)) {
        description.type = RendererBufferDescription::Type::SharedMemory;
        switch (wpe_buffer_shm_get_format(WPE_BUFFER_SHM(buffer))) {
        case WPE_PIXEL_FORMAT_ARGB8888:
#if USE(LIBDRM)
            description.fourcc = DRM_FORMAT_ARGB8888;
#endif
            break;
        }
        description.usage = RendererBufferFormat::Usage::Rendering;
    }
#if OS(ANDROID)
    else if (WPE_IS_BUFFER_ANDROID(buffer)) {
        auto* bufferAndroid = WPE_BUFFER_ANDROID(buffer);
        description.type = RendererBufferDescription::Type::AHardwareBuffer;
        description.fourcc = wpe_buffer_android_get_format(bufferAndroid);
    }
#endif // OS(ANDROID)

    return description;
}

} // namespace WebKit

#endif // ENABLE(WPE_PLATFORM)
