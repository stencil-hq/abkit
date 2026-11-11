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
#include "WebGPUIOSurfaceImageSkia.h"

#if HAVE(WEBGPU_IMPLEMENTATION) && USE(SKIA)

#include "GLContext.h"
#include "PlatformDisplay.h"
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mutex>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RetainPtr.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

#if USE(LIBEPOXY)
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#else
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#endif

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkImageInfo.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLTypes.h>
#include <skia/private/chromium/GrPromiseImageTexture.h>
#include <skia/private/chromium/SkImageChromium.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

static constexpr uint32_t bgraPixelFormat = 'BGRA';
static constexpr uint32_t rgbaPixelFormat = 'RGBA';

static bool isSupportedPixelFormat(IOSurfaceRef surface)
{
    auto pixelFormat = IOSurfaceGetPixelFormat(surface);
    return pixelFormat == bgraPixelFormat || pixelFormat == rgbaPixelFormat;
}

// How the EGL display binds an IOSurface to a texture.
struct IOSurfaceBinding {
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLConfig config { nullptr };
    EGLint eglTextureTarget { EGL_TEXTURE_2D };
    GLenum textureTarget { GL_TEXTURE_2D };
    bool isSupported { false };
};

static const IOSurfaceBinding& ioSurfaceBinding()
{
    static IOSurfaceBinding binding;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        binding.display = PlatformDisplay::sharedDisplay().glDisplay().eglDisplay();
        const char* extensions = eglQueryString(binding.display, EGL_EXTENSIONS);
        if (!GLContext::isExtensionSupported(extensions, "EGL_ANGLE_iosurface_client_buffer"))
            return;

        const EGLint configAttributes[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLint configCount = 0;
        if (!eglChooseConfig(binding.display, configAttributes, &binding.config, 1, &configCount) || configCount != 1)
            return;

        // ANGLE binds an IOSurface to the texture target its configurations name (a 2D texture on Metal).
        eglGetConfigAttrib(binding.display, binding.config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &binding.eglTextureTarget);
        binding.textureTarget = binding.eglTextureTarget == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D;
        binding.isSupported = true;
    });
    return binding;
}

// A texture and the EGL surface that binds an IOSurface to it.
struct IOSurfaceTexture {
    EGLSurface pbuffer { EGL_NO_SURFACE };
    GLuint texture { 0 };

    void destroy()
    {
        auto display = ioSurfaceBinding().display;
        if (texture) {
            eglReleaseTexImage(display, pbuffer, EGL_BACK_BUFFER);
            glDeleteTextures(1, &texture);
        }
        if (pbuffer != EGL_NO_SURFACE)
            eglDestroySurface(display, pbuffer);
    }
};

// Skia may let go of an image on a thread other than the one whose context made its texture. Such
// textures wait here for that context to be current again.
static Lock orphanedTexturesLock;
static Vector<std::pair<Ref<Thread>, IOSurfaceTexture>>& orphanedTextures() WTF_REQUIRES_LOCK(orphanedTexturesLock)
{
    static NeverDestroyed<Vector<std::pair<Ref<Thread>, IOSurfaceTexture>>> textures;
    return textures;
}

static void destroyOrphanedTexturesOfCurrentThread()
{
    Vector<IOSurfaceTexture> textures;
    {
        Locker locker { orphanedTexturesLock };
        orphanedTextures().removeAllMatching([&](auto& entry) {
            if (entry.first.ptr() != &Thread::currentSingleton())
                return false;
            textures.append(entry.second);
            return true;
        });
    }
    for (auto& texture : textures)
        texture.destroy();
}

// What a promise image of an IOSurface keeps until Skia is done with it.
class IOSurfaceTextureSource {
    WTF_MAKE_NONCOPYABLE(IOSurfaceTextureSource);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(IOSurfaceTextureSource);
public:
    explicit IOSurfaceTextureSource(IOSurfaceRef surface)
        : m_surface(surface)
    {
    }

    ~IOSurfaceTextureSource()
    {
        if (!m_thread)
            return;
        if (m_thread == &Thread::currentSingleton() && GLContext::current()) {
            m_texture.destroy();
            return;
        }
        Locker locker { orphanedTexturesLock };
        orphanedTextures().append({ m_thread.releaseNonNull(), m_texture });
    }

    // Called by Skia on the thread of the context that draws the image.
    sk_sp<GrPromiseImageTexture> fulfill()
    {
        auto* glContext = PlatformDisplay::sharedDisplay().skiaGLContext();
        if (!glContext || !glContext->makeContextCurrent())
            return nullptr;

        destroyOrphanedTexturesOfCurrentThread();

        auto& binding = ioSurfaceBinding();
        int width = IOSurfaceGetWidth(m_surface.get());
        int height = IOSurfaceGetHeight(m_surface.get());
        const EGLint attributes[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_IOSURFACE_PLANE_ANGLE, 0,
            EGL_TEXTURE_TARGET, binding.eglTextureTarget,
            EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, IOSurfaceGetPixelFormat(m_surface.get()) == bgraPixelFormat ? GL_BGRA_EXT : GL_RGBA,
            EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
            EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE,
            EGL_NONE
        };
        m_thread = Thread::currentSingleton();
        m_texture.pbuffer = eglCreatePbufferFromClientBuffer(binding.display, EGL_IOSURFACE_ANGLE, m_surface.get(), binding.config, attributes);
        if (m_texture.pbuffer == EGL_NO_SURFACE) {
            WTFLogAlways("Failed to create an EGL surface for the IOSurface of a WebGPU canvas: %s", GLContext::lastErrorString());
            return nullptr;
        }

        // Skia remembers the textures it has bound: leave the unit as it was.
        GLenum bindingName = binding.textureTarget == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_RECTANGLE_ANGLE;
        GLint boundTexture = 0;
        glGetIntegerv(bindingName, &boundTexture);
        glGenTextures(1, &m_texture.texture);
        glBindTexture(binding.textureTarget, m_texture.texture);
        bool bound = eglBindTexImage(binding.display, m_texture.pbuffer, EGL_BACK_BUFFER);
        glBindTexture(binding.textureTarget, boundTexture);
        if (!bound) {
            WTFLogAlways("Failed to bind the IOSurface of a WebGPU canvas to a texture: %s", GLContext::lastErrorString());
            return nullptr;
        }

        GrGLTextureInfo textureInfo;
        textureInfo.fTarget = binding.textureTarget;
        textureInfo.fID = m_texture.texture;
        textureInfo.fFormat = GL_RGBA8;
        return GrPromiseImageTexture::Make(GrBackendTextures::MakeGL(width, height, skgpu::Mipmapped::kNo, textureInfo));
    }

private:
    RetainPtr<IOSurfaceRef> m_surface;
    IOSurfaceTexture m_texture;
    RefPtr<Thread> m_thread;
};

sk_sp<SkImage> createTextureImageFromIOSurface(IOSurfaceRef surface, const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext, bool isOpaque, sk_sp<SkColorSpace>&& colorSpace)
{
    if (!threadSafeGrContext || !isSupportedPixelFormat(surface))
        return nullptr;
    auto& binding = ioSurfaceBinding();
    if (!binding.isSupported)
        return nullptr;

    // The texture samples as RGBA whichever order the surface stores. An opaque canvas leaves the
    // alpha channel of its surface undefined, which the color type without alpha makes Skia ignore.
    auto colorType = isOpaque ? kRGB_888x_SkColorType : kRGBA_8888_SkColorType;
    auto alphaType = isOpaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType;
    auto size = SkISize::Make(IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface));
    return SkImages::PromiseTextureFrom(threadSafeGrContext, GrBackendFormats::MakeGL(GL_RGBA8, binding.textureTarget), size, skgpu::Mipmapped::kNo,
        kTopLeft_GrSurfaceOrigin, colorType, alphaType, WTF::move(colorSpace),
        +[](void* userData) -> sk_sp<GrPromiseImageTexture> {
            return static_cast<IOSurfaceTextureSource*>(userData)->fulfill();
        },
        +[](void* userData) {
            delete static_cast<IOSurfaceTextureSource*>(userData);
        }, new IOSurfaceTextureSource(surface));
}

sk_sp<SkImage> createRasterImageFromIOSurface(IOSurfaceRef surface, bool isOpaque, sk_sp<SkColorSpace>&& colorSpace)
{
    if (!isSupportedPixelFormat(surface))
        return nullptr;
    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess)
        return nullptr;
    auto unlock = makeScopeExit([&] {
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    });

    size_t width = IOSurfaceGetWidth(surface);
    size_t height = IOSurfaceGetHeight(surface);
    size_t surfaceBytesPerRow = IOSurfaceGetBytesPerRow(surface);
    auto colorType = IOSurfaceGetPixelFormat(surface) == bgraPixelFormat ? kBGRA_8888_SkColorType : kRGBA_8888_SkColorType;
    auto info = SkImageInfo::Make(width, height, colorType, isOpaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType, WTF::move(colorSpace));
    size_t bytesPerRow = info.minRowBytes();
    if (surfaceBytesPerRow < bytesPerRow)
        return nullptr;

    auto data = SkData::MakeUninitialized(bytesPerRow * height);
    auto source = unsafeMakeSpan(static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface)), surfaceBytesPerRow * height);
    auto destination = unsafeMakeSpan(static_cast<uint8_t*>(data->writable_data()), data->size());
    for (size_t row = 0; row < height; ++row) {
        auto destinationRow = destination.subspan(row * bytesPerRow, bytesPerRow);
        memcpySpan(destinationRow, source.subspan(row * surfaceBytesPerRow, bytesPerRow));
        if (isOpaque) {
            // The alpha channel of an opaque canvas's surface is undefined.
            for (size_t offset = 3; offset < bytesPerRow; offset += 4)
                destinationRow[offset] = 0xff;
        }
    }
    return SkImages::RasterFromData(info, WTF::move(data), bytesPerRow);
}

} // namespace WebCore

#endif // HAVE(WEBGPU_IMPLEMENTATION) && USE(SKIA)
