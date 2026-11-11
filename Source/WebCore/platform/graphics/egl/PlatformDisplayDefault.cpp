/*
 * Copyright (C) 2024 Igalia S.L.
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
#include "PlatformDisplayDefault.h"

#if PLATFORM(GTK) || OS(ANDROID) || !USE(LIBEPOXY)

#include "GLContext.h"
#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#else
#include <EGL/egl.h>
#if OS(WINDOWS)
#include <EGL/eglext.h>
#endif
#endif

namespace WebCore {

std::unique_ptr<PlatformDisplayDefault> PlatformDisplayDefault::create()
{
    auto glDisplay = GLDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY));
#if OS(WINDOWS)
    // ANGLE's default is a Direct3D 11 hardware device. A machine that has none (a server or a virtual machine
    // without a GPU, a remote session) still has WARP, Direct3D 11's software rasterizer.
    if (!glDisplay) {
        const EGLint warp[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
            EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE,
            EGL_NONE,
        };
        glDisplay = GLDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, warp));
    }
#endif
    if (!glDisplay) {
        WTFLogAlways("Could not create default EGL display: %s. Aborting...", GLContext::lastErrorString());
        CRASH();
    }

    return std::unique_ptr<PlatformDisplayDefault>(new PlatformDisplayDefault(glDisplay.releaseNonNull()));
}

PlatformDisplayDefault::PlatformDisplayDefault(Ref<GLDisplay>&& glDisplay)
    : PlatformDisplay(WTF::move(glDisplay))
{
#if ENABLE(WEBGL)
    m_anglePlatform = 0;
    m_angleNativeDisplay = nullptr;
#endif
}

PlatformDisplayDefault::~PlatformDisplayDefault() = default;

} // namespace WebCore

#endif // PLATFORM(GTK) || OS(ANDROID) || !USE(LIBEPOXY)
