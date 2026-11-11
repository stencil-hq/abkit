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

#if HAVE(WEBGPU_IMPLEMENTATION) && USE(SKIA)

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

typedef struct __IOSurface* IOSurfaceRef;

namespace WebCore {

// The render buffers of a WebGPU canvas are IOSurfaces of 8-bit BGRA or RGBA pixels. These make
// Skia images of them: one for the compositor, one for reading the canvas back.

// An image the context textures from the surface itself, without a copy: it shows what the surface
// holds when the image is drawn. Null when the context has no way to bind an IOSurface.
sk_sp<SkImage> createTextureImageFromIOSurface(IOSurfaceRef, const sk_sp<GrContextThreadSafeProxy>&, bool isOpaque, sk_sp<SkColorSpace>&&);

// A copy of the pixels the surface holds now.
sk_sp<SkImage> createRasterImageFromIOSurface(IOSurfaceRef, bool isOpaque, sk_sp<SkColorSpace>&&);

} // namespace WebCore

#endif // HAVE(WEBGPU_IMPLEMENTATION) && USE(SKIA)
