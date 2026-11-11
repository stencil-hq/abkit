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
#include "WebKitWindowsTLSDatabase.h"

#if OS(WINDOWS)

#include <windows.h>
#include <wincrypt.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/WTFString.h>

// glib-networking's OpenSSL backend trusts the roots it read from Windows'
// store when it started. Windows installs most of the roots it trusts only
// when its own chain building first meets one (automatic root update), which
// OpenSSL never triggers: sites under such a root failed as signed by an
// unknown authority. A chain failing for that reason alone is verified again
// as Windows verifies a TLS server, and trusted only if that succeeds.

typedef struct _WebKitWindowsTLSDatabase {
    GTlsDatabase parent;
    GTlsDatabase* system;
} WebKitWindowsTLSDatabase;

typedef struct _WebKitWindowsTLSDatabaseClass {
    GTlsDatabaseClass parentClass;
} WebKitWindowsTLSDatabaseClass;

G_DEFINE_TYPE(WebKitWindowsTLSDatabase, webkit_windows_tls_database, G_TYPE_TLS_DATABASE)

static GTlsDatabase* systemDatabase(GTlsDatabase* database)
{
    return reinterpret_cast<WebKitWindowsTLSDatabase*>(database)->system;
}

// The name the server must hold a certificate for.
static String serverName(GSocketConnectable* identity)
{
    if (G_IS_NETWORK_ADDRESS(identity))
        return String::fromUTF8(g_network_address_get_hostname(G_NETWORK_ADDRESS(identity)));
    if (G_IS_NETWORK_SERVICE(identity))
        return String::fromUTF8(g_network_service_get_domain(G_NETWORK_SERVICE(identity)));
    if (G_IS_INET_SOCKET_ADDRESS(identity)) {
        GUniquePtr<char> address(g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(identity))));
        return String::fromUTF8(address.get());
    }
    return { };
}

// Whether Windows trusts `chain` (the server's certificate, then the issuers
// it sent) for a TLS server named `name`: CryptoAPI builds the chain from the
// system's stores and the certificates sent, fetching what it lacks, then
// checks it against the SSL policy (trust, validity, usage, name).
static bool windowsTrustsServerChain(GTlsCertificate* chain, const String& name)
{
    HCERTSTORE sent = ::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!sent)
        return false;

    PCCERT_CONTEXT server = nullptr;
    for (GTlsCertificate* certificate = chain; certificate; certificate = g_tls_certificate_get_issuer(certificate)) {
        GRefPtr<GByteArray> der;
        g_object_get(certificate, "certificate", &der.outPtr(), nullptr);
        if (!der || !::CertAddEncodedCertificateToStore(sent, X509_ASN_ENCODING, der->data, der->len, CERT_STORE_ADD_ALWAYS, certificate == chain ? &server : nullptr))
            break;
    }

    bool trusted = false;
    if (server) {
        LPSTR usages[] = { const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH) };
        CERT_CHAIN_PARA chainParameters { };
        chainParameters.cbSize = sizeof(chainParameters);
        chainParameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        chainParameters.RequestedUsage.Usage.cUsageIdentifier = std::size(usages);
        chainParameters.RequestedUsage.Usage.rgpszUsageIdentifier = usages;

        PCCERT_CHAIN_CONTEXT built = nullptr;
        if (::CertGetCertificateChain(nullptr, server, nullptr, sent, &chainParameters, 0, nullptr, &built)) {
            auto wideName = name.wideCharacters();
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslParameters { };
            sslParameters.cbSize = sizeof(sslParameters);
            sslParameters.dwAuthType = AUTHTYPE_SERVER;
            sslParameters.pwszServerName = wideName.mutableSpan().data();

            CERT_CHAIN_POLICY_PARA policyParameters { };
            policyParameters.cbSize = sizeof(policyParameters);
            policyParameters.pvExtraPolicyPara = &sslParameters;

            CERT_CHAIN_POLICY_STATUS status { };
            status.cbSize = sizeof(status);
            trusted = ::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, built, &policyParameters, &status) && !status.dwError;
            ::CertFreeCertificateChain(built);
        }
        ::CertFreeCertificateContext(server);
    }
    ::CertCloseStore(sent, 0);
    return trusted;
}

static GTlsCertificateFlags webkitWindowsTLSDatabaseVerifyChain(GTlsDatabase* database, GTlsCertificate* chain, const char* purpose, GSocketConnectable* identity, GTlsInteraction* interaction, GTlsDatabaseVerifyFlags flags, GCancellable* cancellable, GError** error)
{
    GUniqueOutPtr<GError> verifyError;
    auto errors = g_tls_database_verify_chain(systemDatabase(database), chain, purpose, identity, interaction, flags, cancellable, &verifyError.outPtr());
    if (verifyError) {
        g_propagate_error(error, verifyError.release());
        return errors;
    }

    if (errors != G_TLS_CERTIFICATE_UNKNOWN_CA || g_strcmp0(purpose, G_TLS_DATABASE_PURPOSE_AUTHENTICATE_SERVER) || !identity)
        return errors;

    auto name = serverName(identity);
    if (name.isEmpty() || g_cancellable_is_cancelled(cancellable) || !windowsTrustsServerChain(chain, name))
        return errors;
    return static_cast<GTlsCertificateFlags>(0);
}

static char* webkitWindowsTLSDatabaseCreateCertificateHandle(GTlsDatabase* database, GTlsCertificate* certificate)
{
    return g_tls_database_create_certificate_handle(systemDatabase(database), certificate);
}

static GTlsCertificate* webkitWindowsTLSDatabaseLookupCertificateForHandle(GTlsDatabase* database, const char* handle, GTlsInteraction* interaction, GTlsDatabaseLookupFlags flags, GCancellable* cancellable, GError** error)
{
    return g_tls_database_lookup_certificate_for_handle(systemDatabase(database), handle, interaction, flags, cancellable, error);
}

static GTlsCertificate* webkitWindowsTLSDatabaseLookupCertificateIssuer(GTlsDatabase* database, GTlsCertificate* certificate, GTlsInteraction* interaction, GTlsDatabaseLookupFlags flags, GCancellable* cancellable, GError** error)
{
    return g_tls_database_lookup_certificate_issuer(systemDatabase(database), certificate, interaction, flags, cancellable, error);
}

static GList* webkitWindowsTLSDatabaseLookupCertificatesIssuedBy(GTlsDatabase* database, GByteArray* issuer, GTlsInteraction* interaction, GTlsDatabaseLookupFlags flags, GCancellable* cancellable, GError** error)
{
    return g_tls_database_lookup_certificates_issued_by(systemDatabase(database), issuer, interaction, flags, cancellable, error);
}

static void webkitWindowsTLSDatabaseFinalize(GObject* object)
{
    g_clear_object(&reinterpret_cast<WebKitWindowsTLSDatabase*>(object)->system);
    G_OBJECT_CLASS(webkit_windows_tls_database_parent_class)->finalize(object);
}

static void webkit_windows_tls_database_class_init(WebKitWindowsTLSDatabaseClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = webkitWindowsTLSDatabaseFinalize;

    // The asynchronous variants are GTlsDatabase's: these in a thread.
    auto* databaseClass = G_TLS_DATABASE_CLASS(klass);
    databaseClass->verify_chain = webkitWindowsTLSDatabaseVerifyChain;
    databaseClass->create_certificate_handle = webkitWindowsTLSDatabaseCreateCertificateHandle;
    databaseClass->lookup_certificate_for_handle = webkitWindowsTLSDatabaseLookupCertificateForHandle;
    databaseClass->lookup_certificate_issuer = webkitWindowsTLSDatabaseLookupCertificateIssuer;
    databaseClass->lookup_certificates_issued_by = webkitWindowsTLSDatabaseLookupCertificatesIssuedBy;
}

static void webkit_windows_tls_database_init(WebKitWindowsTLSDatabase*)
{
}

GRefPtr<GTlsDatabase> webkitWindowsTLSDatabaseNew(GTlsDatabase* systemDatabase)
{
    auto* database = reinterpret_cast<WebKitWindowsTLSDatabase*>(g_object_new(webkit_windows_tls_database_get_type(), nullptr));
    database->system = G_TLS_DATABASE(g_object_ref(systemDatabase));
    return adoptGRef(G_TLS_DATABASE(database));
}

#endif // OS(WINDOWS)
