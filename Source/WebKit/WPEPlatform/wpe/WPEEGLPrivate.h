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

// EGL for WPE Platform: libepoxy's where the system has an EGL of its own,
// else the EGL the build links (ANGLE's on Darwin and Windows) with the three
// libepoxy helpers the platform code uses.

#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wtf/text/StringView.h>

static inline bool epoxy_has_egl_extension(EGLDisplay display, const char* extension)
{
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!extensions)
        return false;
    auto extensionsView = StringView::fromLatin1(extensions);
    auto extensionView = StringView::fromLatin1(extension);
    for (size_t at = extensionsView.find(extensionView); at != WTF::notFound; at = extensionsView.find(extensionView, at + extensionView.length())) {
        bool startsName = !at || extensionsView[at - 1] == ' ';
        auto end = at + extensionView.length();
        bool endsName = end == extensionsView.length() || extensionsView[end] == ' ';
        if (startsName && endsName)
            return true;
    }
    return false;
}

static inline void* epoxy_eglGetProcAddress(const char* name)
{
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

// As libepoxy: major * 10 + minor.
static inline int epoxy_egl_version(EGLDisplay display)
{
    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(display, &major, &minor))
        return 0;
    return major * 10 + minor;
}
#endif
