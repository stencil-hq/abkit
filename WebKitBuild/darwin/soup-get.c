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

// Fetches URLs with the libsoup and the TLS backend build-deps.sh built and prints the last body, to
// check what a server sees from them without the engine.
// Usage: soup-get [--insecure] URL...   (--insecure accepts any certificate)
// Each URL is fetched in turn by one session, so a later one meets the connection or the TLS session
// of an earlier one. The TLS backend in use, each response's status and each connection's
// certificate errors go to stderr.

#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>

static gboolean acceptCertificate(SoupMessage* message, GTlsCertificate* certificate, GTlsCertificateFlags errors, gpointer insecure)
{
    fprintf(stderr, "certificate errors: 0x%x\n", errors);
    return GPOINTER_TO_INT(insecure);
}

int main(int argc, char** argv)
{
    gboolean insecure = argc > 1 && !strcmp(argv[1], "--insecure");
    int first = insecure ? 2 : 1;
    if (first >= argc) {
        fprintf(stderr, "usage: %s [--insecure] URL...\n", argv[0]);
        return 64;
    }

    fprintf(stderr, "TLS backend: %s\n", G_OBJECT_TYPE_NAME(g_tls_backend_get_default()));

    SoupSession* session = soup_session_new();
    GBytes* body = NULL;
    for (int i = first; i < argc; i++) {
        SoupMessage* message = soup_message_new("GET", argv[i]);
        if (!message) {
            fprintf(stderr, "%s: not a URL\n", argv[i]);
            return 64;
        }
        g_signal_connect(message, "accept-certificate", G_CALLBACK(acceptCertificate), GINT_TO_POINTER(insecure));

        GError* error = NULL;
        g_clear_pointer(&body, g_bytes_unref);
        body = soup_session_send_and_read(session, message, NULL, &error);
        if (!body) {
            fprintf(stderr, "%s: %s\n", argv[i], error->message);
            return 1;
        }
        fprintf(stderr, "%s: %u, HTTP/%s, %s\n", argv[i], soup_message_get_status(message),
            soup_message_get_http_version(message) == SOUP_HTTP_2_0 ? "2" : "1.1",
            soup_message_get_tls_ciphersuite_name(message) ? soup_message_get_tls_ciphersuite_name(message) : "no TLS");
        g_object_unref(message);
    }

    fwrite(g_bytes_get_data(body, NULL), 1, g_bytes_get_size(body), stdout);
    g_bytes_unref(body);
    g_object_unref(session);
    return 0;
}
