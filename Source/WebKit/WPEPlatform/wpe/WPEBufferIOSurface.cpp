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
#include "WPEBufferIOSurface.h"

#if OS(DARWIN)

#include "WPEBufferPrivate.h"
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/WTFGType.h>

/**
 * WPEBufferIOSurface:
 *
 * A [class@Buffer] backed by an `IOSurface`.
 *
 * The web process renders into the surface with the GPU and hands it over
 * without copying: a host shows it as is (e.g. as a `CALayer`'s contents).
 * Its pixels are 32-bit BGRA, premultiplied alpha, sRGB, top row first.
 * Only available on Darwin.
 */
struct _WPEBufferIOSurfacePrivate {
    IOSurfaceRef surface;
    GRefPtr<GBytes> pixels;
};
WEBKIT_DEFINE_FINAL_TYPE(WPEBufferIOSurface, wpe_buffer_iosurface, WPE_TYPE_BUFFER, WPEBuffer)

static void wpeBufferIOSurfaceDispose(GObject* object)
{
    auto* priv = WPE_BUFFER_IOSURFACE(object)->priv;
    if (priv->surface) {
        CFRelease(priv->surface);
        priv->surface = nullptr;
    }
    priv->pixels = nullptr;

    G_OBJECT_CLASS(wpe_buffer_iosurface_parent_class)->dispose(object);
}

static GBytes* wpeBufferIOSurfaceImportToPixels(WPEBuffer* buffer, GError** error)
{
    auto* priv = WPE_BUFFER_IOSURFACE(buffer)->priv;
    // Locking waits for the GPU's writes to the surface to land.
    if (IOSurfaceLock(priv->surface, kIOSurfaceLockReadOnly, nullptr)) {
        g_set_error_literal(error, WPE_BUFFER_ERROR, WPE_BUFFER_ERROR_IMPORT_FAILED, "Failed to import buffer to pixels: the IOSurface cannot be locked");
        return nullptr;
    }
    auto width = IOSurfaceGetWidth(priv->surface);
    auto height = IOSurfaceGetHeight(priv->surface);
    auto stride = IOSurfaceGetBytesPerRow(priv->surface);
    auto data = unsafeMakeSpan(static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(priv->surface)), stride * height);
    // The returned rows must not be padded, and must outlive the lock.
    priv->pixels = wpeBufferCopyPixelsWithoutRowPadding(data, width, height, stride);
    IOSurfaceUnlock(priv->surface, kIOSurfaceLockReadOnly, nullptr);
    if (!priv->pixels) {
        g_set_error_literal(error, WPE_BUFFER_ERROR, WPE_BUFFER_ERROR_IMPORT_FAILED, "Failed to import buffer to pixels: invalid IOSurface stride");
        return nullptr;
    }
    return priv->pixels.get();
}

static void wpe_buffer_iosurface_class_init(WPEBufferIOSurfaceClass* bufferIOSurfaceClass)
{
    GObjectClass* objectClass = G_OBJECT_CLASS(bufferIOSurfaceClass);
    objectClass->dispose = wpeBufferIOSurfaceDispose;

    WPEBufferClass* bufferClass = WPE_BUFFER_CLASS(bufferIOSurfaceClass);
    bufferClass->import_to_pixels = wpeBufferIOSurfaceImportToPixels;
}

/**
 * wpe_buffer_iosurface_new:
 * @display: a #WPEDisplay
 * @surface: an `IOSurfaceRef`
 *
 * Create a new #WPEBufferIOSurface for @surface, which it retains. Its width
 * and height are the surface's.
 *
 * Returns: (transfer full): a #WPEBufferIOSurface
 */
WPEBufferIOSurface* wpe_buffer_iosurface_new(WPEDisplay* display, IOSurfaceRef surface)
{
    g_return_val_if_fail(WPE_IS_DISPLAY(display), nullptr);
    g_return_val_if_fail(surface, nullptr);

    auto* buffer = WPE_BUFFER_IOSURFACE(g_object_new(WPE_TYPE_BUFFER_IOSURFACE,
        "display", display,
        "width", static_cast<int>(IOSurfaceGetWidth(surface)),
        "height", static_cast<int>(IOSurfaceGetHeight(surface)),
        nullptr));
    buffer->priv->surface = static_cast<IOSurfaceRef>(const_cast<void*>(CFRetain(surface)));
    return buffer;
}

/**
 * wpe_buffer_iosurface_get_surface:
 * @buffer: a #WPEBufferIOSurface
 *
 * Get the `IOSurfaceRef` of @buffer, owned by @buffer: retain it to keep it
 * beyond the buffer.
 *
 * Returns: (transfer none): an `IOSurfaceRef`
 */
IOSurfaceRef wpe_buffer_iosurface_get_surface(WPEBufferIOSurface* buffer)
{
    g_return_val_if_fail(WPE_IS_BUFFER_IOSURFACE(buffer), nullptr);

    return buffer->priv->surface;
}

#endif // OS(DARWIN)
