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

#include "gtlsbackend-openssl.h"
#include <gio/gio.h>

// GIO finds a module's entry points by the name its file has after "libgio".

__attribute__((visibility("default"))) void g_io_boringssl_load(GIOModule* module)
{
    // GIO registers its extension points when it first needs one, which may be after a process
    // has loaded this module by hand.
    GIOExtensionPoint* point = g_io_extension_point_register(G_TLS_BACKEND_EXTENSION_POINT_NAME);
    g_io_extension_point_set_required_type(point, G_TYPE_TLS_BACKEND);

    g_tls_backend_openssl_register(module);
}

__attribute__((visibility("default"))) void g_io_boringssl_unload(GIOModule* module)
{
}

__attribute__((visibility("default"))) gchar** g_io_boringssl_query(void)
{
    return g_strsplit(G_TLS_BACKEND_EXTENSION_POINT_NAME, "!", -1);
}
