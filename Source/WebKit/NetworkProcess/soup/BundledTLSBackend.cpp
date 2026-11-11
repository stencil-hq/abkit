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
#include "BundledTLSBackend.h"

#if OS(DARWIN) || OS(LINUX)

#include "ProcessExecutablePath.h"
#include <gio/gio.h>
#include <wtf/FileSystem.h>
#include <wtf/text/CString.h>

namespace WebKit {

void loadBundledTLSBackend()
{
    auto path = FileSystem::fileSystemRepresentation(FileSystem::pathByAppendingComponent(directoryOfWebKitLibrary(), "libgioboringssl.so"_s));
    auto* module = g_io_module_new(path.legacyCStringPointer());

    // A used module is never unloaded, which is what a TLS backend needs; a file that is missing or
    // does not load leaves GIO with the backends it finds installed, whose ClientHello is not Safari's.
    if (!g_type_module_use(G_TYPE_MODULE(module))) {
        g_warning("Could not load the TLS backend %s; connections will not look like Safari's", path.legacyCStringPointer());
        g_object_unref(module);
    }
}

} // namespace WebKit

#endif // OS(DARWIN) || OS(LINUX)
