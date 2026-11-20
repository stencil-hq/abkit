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

#if USE(ANGLE) && USE(GLIB)

typedef void* EGLDisplay;

namespace WebCore {

// Compiled shaders and programs of ANGLE's displays, kept on disk across processes and launches
// (EGL_ANDROID_blob_cache): a page's WebGL programs and ANGLE's own skip compiling again (Direct3D
// bytecode, GL program binaries, translated Metal source) in the next web process that needs them.
// Kept in the user's cache directory (`<cache>/wpe/shader-cache`, `webkitgtk` for GTK), at most
// 128 MiB, oldest out first. Only an ANGLE display has one: a system EGL keeps its own.
namespace GLShaderCache {

// Gives ANGLE display `display`, initialized, the cache, when it has the extension.
void attach(EGLDisplay);

// Whether this process keeps what it compiles (false for a process of an ephemeral session, which
// still reads what others kept); false until said.
WEBCORE_EXPORT void setPersistent(bool);

} // namespace GLShaderCache

} // namespace WebCore

#endif // USE(ANGLE) && USE(GLIB)
