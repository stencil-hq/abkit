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
#include "GTlsDatabaseSecTrust.h"

#include "openssl-include.h"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

struct _GTlsDatabaseSecTrust {
    GTlsDatabaseOpenssl parent_instance;
};

G_DEFINE_FINAL_TYPE(GTlsDatabaseSecTrust, g_tls_database_sec_trust, G_TYPE_TLS_DATABASE_OPENSSL)

static CFStringRef copyHostName(GSocketConnectable* identity)
{
    const char* name = NULL;
    char* address = NULL;
    if (G_IS_NETWORK_ADDRESS(identity))
        name = g_network_address_get_hostname(G_NETWORK_ADDRESS(identity));
    else if (G_IS_NETWORK_SERVICE(identity))
        name = g_network_service_get_domain(G_NETWORK_SERVICE(identity));
    else if (G_IS_INET_SOCKET_ADDRESS(identity))
        name = address = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(identity)));

    CFStringRef result = name ? CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8) : NULL;
    g_free(address);
    return result;
}

static CFArrayRef copyCertificates(GTlsCertificate* chain)
{
    CFMutableArrayRef certificates = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (GTlsCertificate* link = chain; link; link = g_tls_certificate_get_issuer(link)) {
        uint8_t* encoded = NULL;
        int length = i2d_X509(g_tls_certificate_openssl_get_cert(G_TLS_CERTIFICATE_OPENSSL(link)), &encoded);
        SecCertificateRef certificate = NULL;
        if (length > 0) {
            CFDataRef data = CFDataCreate(NULL, encoded, length);
            certificate = SecCertificateCreateWithData(NULL, data);
            CFRelease(data);
            OPENSSL_free(encoded);
        }
        if (!certificate) {
            CFRelease(certificates);
            return NULL;
        }
        CFArrayAppendValue(certificates, certificate);
        CFRelease(certificate);
    }
    return certificates;
}

// The extension carries a list of timestamps, each behind a two-byte length, behind a two-byte length
// of its own (RFC 6962, section 3.3). The framework takes the timestamps one by one.
static CFArrayRef copySignedCertificateTimestamps(const guint8* list, gsize length)
{
    if (length < 2 || (gsize)((list[0] << 8) | list[1]) != length - 2)
        return NULL;

    CFMutableArrayRef timestamps = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    gsize position = 2;
    while (position + 2 <= length) {
        gsize size = (list[position] << 8) | list[position + 1];
        position += 2;
        if (!size || size > length - position) {
            CFRelease(timestamps);
            return NULL;
        }
        CFDataRef timestamp = CFDataCreate(NULL, list + position, size);
        CFArrayAppendValue(timestamps, timestamp);
        CFRelease(timestamp);
        position += size;
    }
    return timestamps;
}

static GTlsCertificateFlags flagsForTrustFailure(OSStatus status)
{
    switch (status) {
    case errSecHostNameMismatch:
        return G_TLS_CERTIFICATE_BAD_IDENTITY;
    case errSecCertificateExpired:
        return G_TLS_CERTIFICATE_EXPIRED;
    case errSecCertificateNotValidYet:
        return G_TLS_CERTIFICATE_NOT_ACTIVATED;
    case errSecCertificateRevoked:
        return G_TLS_CERTIFICATE_REVOKED;
    case errSecNotTrusted:
    case errSecCreateChainFailed:
    case errSecTrustSettingDeny:
    case errSecInvalidCertAuthority:
        return G_TLS_CERTIFICATE_UNKNOWN_CA;
    case errSecInvalidDigestAlgorithm:
    case errSecUnsupportedKeySize:
        return G_TLS_CERTIFICATE_INSECURE;
    default:
        return G_TLS_CERTIFICATE_GENERIC_ERROR;
    }
}

GTlsCertificateFlags g_tls_database_sec_trust_evaluate(GTlsCertificate* chain, const gchar* purpose, GSocketConnectable* identity, const guint8* ocspResponse, gsize ocspResponseLength, const guint8* sctList, gsize sctListLength)
{
    g_return_val_if_fail(G_IS_TLS_CERTIFICATE_OPENSSL(chain), G_TLS_CERTIFICATE_GENERIC_ERROR);

    CFArrayRef certificates = copyCertificates(chain);
    if (!certificates)
        return G_TLS_CERTIFICATE_GENERIC_ERROR;

    SecPolicyRef policy;
    if (!g_strcmp0(purpose, G_TLS_DATABASE_PURPOSE_AUTHENTICATE_SERVER)) {
        CFStringRef hostName = identity ? copyHostName(identity) : NULL;
        policy = SecPolicyCreateSSL(true, hostName);
        if (hostName)
            CFRelease(hostName);
    } else if (!g_strcmp0(purpose, G_TLS_DATABASE_PURPOSE_AUTHENTICATE_CLIENT))
        policy = SecPolicyCreateSSL(false, NULL);
    else
        policy = SecPolicyCreateBasicX509();

    SecTrustRef trust = NULL;
    OSStatus status = SecTrustCreateWithCertificates(certificates, policy, &trust);
    CFRelease(policy);
    CFRelease(certificates);
    if (status != errSecSuccess || !trust)
        return G_TLS_CERTIFICATE_GENERIC_ERROR;

    if (ocspResponseLength) {
        CFDataRef response = CFDataCreate(NULL, ocspResponse, ocspResponseLength);
        SecTrustSetOCSPResponse(trust, response);
        CFRelease(response);
    }

    if (sctListLength) {
        CFArrayRef timestamps = copySignedCertificateTimestamps(sctList, sctListLength);
        if (timestamps) {
            SecTrustSetSignedCertificateTimestamps(trust, timestamps);
            CFRelease(timestamps);
        }
    }

    GTlsCertificateFlags flags = 0;
    CFErrorRef error = NULL;
    if (!SecTrustEvaluateWithError(trust, &error)) {
        flags = flagsForTrustFailure(error ? (OSStatus)CFErrorGetCode(error) : errSecNotTrusted);

        // The framework names one reason. The leaf's own names and dates tell whether the others
        // a caller distinguishes apply as well.
        X509* leaf = g_tls_certificate_openssl_get_cert(G_TLS_CERTIFICATE_OPENSSL(chain));
        if (identity)
            flags |= g_tls_certificate_openssl_verify_identity(G_TLS_CERTIFICATE_OPENSSL(chain), identity);
        if (X509_cmp_current_time(X509_get0_notBefore(leaf)) > 0)
            flags |= G_TLS_CERTIFICATE_NOT_ACTIVATED;
        if (X509_cmp_current_time(X509_get0_notAfter(leaf)) < 0)
            flags |= G_TLS_CERTIFICATE_EXPIRED;
    }
    if (error)
        CFRelease(error);
    CFRelease(trust);
    return flags;
}

static GTlsCertificateFlags g_tls_database_sec_trust_verify_chain(GTlsDatabase* database, GTlsCertificate* chain, const gchar* purpose, GSocketConnectable* identity, GTlsInteraction* interaction, GTlsDatabaseVerifyFlags flags, GCancellable* cancellable, GError** error)
{
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
        return G_TLS_CERTIFICATE_GENERIC_ERROR;

    return g_tls_database_sec_trust_evaluate(chain, purpose, identity, NULL, 0, NULL, 0);
}

static gboolean g_tls_database_sec_trust_populate_trust_list(GTlsDatabaseOpenssl* database, X509_STORE* store, GError** error)
{
    // The anchors stay in the keychains, where the Security framework reads them with their trust settings.
    return TRUE;
}

static void g_tls_database_sec_trust_class_init(GTlsDatabaseSecTrustClass* klass)
{
    G_TLS_DATABASE_CLASS(klass)->verify_chain = g_tls_database_sec_trust_verify_chain;
    G_TLS_DATABASE_OPENSSL_CLASS(klass)->populate_trust_list = g_tls_database_sec_trust_populate_trust_list;
}

static void g_tls_database_sec_trust_init(GTlsDatabaseSecTrust* database)
{
}

GTlsDatabase* g_tls_database_sec_trust_new(GError** error)
{
    return g_initable_new(G_TYPE_TLS_DATABASE_SEC_TRUST, NULL, error, NULL);
}
