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

#include "SafariClientHello.h"

#include <zlib.h>

// The values below are what system WebKit sends on macOS 27 (Safari 27), in the order it sends them.
// BoringSSL writes the extensions in the order Apple's TLS library does, so only their content is set.

static const char safariTLS12CipherSuites[] =
    "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:"
    "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:"
    "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256:"
    "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384:"
    "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:"
    "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256:"
    "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA:"
    "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA:"
    "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA:"
    "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA:"
    "TLS_RSA_WITH_AES_256_GCM_SHA384:"
    "TLS_RSA_WITH_AES_128_GCM_SHA256:"
    "TLS_RSA_WITH_AES_256_CBC_SHA:"
    "TLS_RSA_WITH_AES_128_CBC_SHA:"
    "TLS_ECDHE_ECDSA_WITH_3DES_EDE_CBC_SHA:"
    "TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA:"
    "TLS_RSA_WITH_3DES_EDE_CBC_SHA";

static const uint16_t safariTLS13CipherSuites[] = {
    SSL_CIPHER_AES_256_GCM_SHA384,
    SSL_CIPHER_CHACHA20_POLY1305_SHA256,
    SSL_CIPHER_AES_128_GCM_SHA256,
};

static const uint16_t safariGroups[] = {
    SSL_GROUP_X25519_MLKEM768,
    SSL_GROUP_X25519,
    SSL_GROUP_SECP256R1,
    SSL_GROUP_SECP384R1,
    SSL_GROUP_SECP521R1,
};

// Safari lists rsa_pss_rsae_sha384 twice.
static const uint16_t safariSignatureAlgorithms[] = {
    SSL_SIGN_ECDSA_SECP256R1_SHA256,
    SSL_SIGN_RSA_PSS_RSAE_SHA256,
    SSL_SIGN_RSA_PKCS1_SHA256,
    SSL_SIGN_ECDSA_SECP384R1_SHA384,
    SSL_SIGN_RSA_PSS_RSAE_SHA384,
    SSL_SIGN_RSA_PSS_RSAE_SHA384,
    SSL_SIGN_RSA_PKCS1_SHA384,
    SSL_SIGN_RSA_PSS_RSAE_SHA512,
    SSL_SIGN_RSA_PKCS1_SHA512,
    SSL_SIGN_RSA_PKCS1_SHA1,
};

static int decompressCertificate(SSL* ssl, CRYPTO_BUFFER** out, size_t uncompressedLength, const uint8_t* in, size_t inLength)
{
    uint8_t* data;
    CRYPTO_BUFFER* buffer = CRYPTO_BUFFER_alloc(&data, uncompressedLength);
    if (!buffer)
        return 0;

    uLongf length = uncompressedLength;
    if (uncompress(data, &length, in, inLength) != Z_OK || length != uncompressedLength) {
        CRYPTO_BUFFER_free(buffer);
        return 0;
    }

    *out = buffer;
    return 1;
}

gboolean g_tls_boringssl_configure_safari_client_hello(SSL_CTX* context)
{
    SSL_CTX_set_grease_enabled(context, 1);

    if (!SSL_CTX_set_strict_cipher_list(context, safariTLS12CipherSuites)
        || !SSL_CTX_set_tls13_cipher_order(context, safariTLS13CipherSuites, G_N_ELEMENTS(safariTLS13CipherSuites))
        || !SSL_CTX_set1_group_ids(context, safariGroups, G_N_ELEMENTS(safariGroups))
        || !SSL_CTX_set_verify_algorithm_prefs(context, safariSignatureAlgorithms, G_N_ELEMENTS(safariSignatureAlgorithms)))
        return FALSE;

    // Safari resumes TLS 1.2 sessions by their ID and sends no session_ticket extension.
    SSL_CTX_set_options(context, SSL_OP_NO_TICKET);

    SSL_CTX_enable_ocsp_stapling(context);
    SSL_CTX_enable_signed_cert_timestamps(context);

    // A client only decompresses the server's certificate, so there is no compression function.
    return SSL_CTX_add_cert_compression_alg(context, TLSEXT_cert_compression_zlib, NULL, decompressCertificate);
}
