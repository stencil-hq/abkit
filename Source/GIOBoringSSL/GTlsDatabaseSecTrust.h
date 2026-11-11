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

#include "gtlsdatabase-openssl.h"

G_BEGIN_DECLS

// The backend's default database on Apple systems. The trust decision is the Security framework's:
// the keychains' anchors and trust settings and the policy Safari applies to a server certificate
// (hostname, validity, revocation, Certificate Transparency). It holds no certificates of its own.
#define G_TYPE_TLS_DATABASE_SEC_TRUST (g_tls_database_sec_trust_get_type())

G_DECLARE_FINAL_TYPE(GTlsDatabaseSecTrust, g_tls_database_sec_trust, G, TLS_DATABASE_SEC_TRUST, GTlsDatabaseOpenssl)

GTlsDatabase* g_tls_database_sec_trust_new(GError**);

// Evaluates a peer's chain. The stapled OCSP response and the signed certificate timestamp list are
// the ones the peer sent in the handshake, when it sent any. May block while the trust daemon
// fetches a missing intermediate or a revocation status, so it is not for the main thread.
GTlsCertificateFlags g_tls_database_sec_trust_evaluate(GTlsCertificate* chain, const gchar* purpose, GSocketConnectable* identity, const guint8* ocspResponse, gsize ocspResponseLength, const guint8* sctList, gsize sctListLength);

G_END_DECLS
