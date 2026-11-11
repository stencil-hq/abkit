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
#include "GStreamerScreenDeviceDarwin.h"

#if ENABLE(MEDIA_STREAM) && USE(GSTREAMER) && OS(DARWIN)

#include "GStreamerCommon.h"
#include <wtf/glib/WTFGType.h>

using namespace WebCore;

typedef struct _WebKitScreenDevice WebKitScreenDevice;
typedef struct _WebKitScreenDeviceClass WebKitScreenDeviceClass;
typedef struct _WebKitScreenDevicePrivate WebKitScreenDevicePrivate;

struct _WebKitScreenDevice {
    GstDevice parent;
    WebKitScreenDevicePrivate* priv;
};

struct _WebKitScreenDeviceClass {
    GstDeviceClass parentClass;
};

struct _WebKitScreenDevicePrivate {
};

GType webkit_screen_device_get_type(void);

WEBKIT_DEFINE_TYPE(WebKitScreenDevice, webkit_screen_device, GST_TYPE_DEVICE)

static GstElement* webkitScreenDeviceCreateElement(GstDevice*, const char* name)
{
    auto* element = makeGStreamerElement("avfvideosrc"_s, String::fromLatin1(name));
    if (element)
        g_object_set(element, "capture-screen", TRUE, "capture-screen-cursor", TRUE, nullptr);
    return element;
}

static void webkit_screen_device_class_init(WebKitScreenDeviceClass* klass)
{
    GST_DEVICE_CLASS(klass)->create_element = GST_DEBUG_FUNCPTR(webkitScreenDeviceCreateElement);
}

namespace WebCore {

GRefPtr<GstDevice> createScreenCaptureDevice()
{
    GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("video/x-raw"));
    auto* device = GST_DEVICE_CAST(g_object_new(webkit_screen_device_get_type(), "display-name", "Screen", "device-class", "Video/Source", "caps", caps.get(), nullptr));
    gst_object_ref_sink(device);
    return adoptGRef(device);
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER) && OS(DARWIN)
