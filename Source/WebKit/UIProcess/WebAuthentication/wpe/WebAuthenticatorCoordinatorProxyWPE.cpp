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
#include "WebAuthenticatorCoordinatorProxy.h"

#if ENABLE(WEB_AUTHN)

#include "AuthenticatorManager.h"
#include "Logging.h"
#include "RelatedOriginsValidator.h"
#include "ValidationProcedures.h"
#include "WebAuthenticationRequestData.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebProcessProxy.h"
#include "WebsiteDataStore.h"
#include <WebCore/AllAcceptedCredentialsOptions.h>
#include <WebCore/AuthenticatorResponse.h>
#include <WebCore/AuthenticatorResponseData.h>
#include <WebCore/CurrentUserDetailsOptions.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/RegistrableDomain.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/UnknownCredentialOptions.h>
#include <WebCore/WebAuthenticationUtils.h>
#include <wtf/text/Base64.h>

// FIXME: https://bugs.webkit.org/show_bug.cgi?id=325600 Compare against the calling frame's committed origin, not the sender's domain authority.
#define EXTRACT_WITH_MESSAGE_CHECK_COMPLETION(name, untrusted, completion, ...) \
    EXTRACT_WITH_MESSAGE_CHECK_BASE(connection, name, untrusted, completion, __VA_ARGS__)

namespace WebKit {
using namespace WebCore;

static String relyingPartyIdentifierForRequest(const WebAuthenticationRequestData& requestData)
{
    return WTF::switchOn(requestData.options, [](const PublicKeyCredentialCreationOptions& options) {
        return options.rp.id;
    }, [](const PublicKeyCredentialRequestOptions& options) {
        return options.rpId;
    });
}

// The steps of a request that come before any authenticator is involved: the relying party must be
// the caller's own registrable domain or one that lists the caller as a related origin.
void WebAuthenticatorCoordinatorProxy::performRequest(WebAuthenticationRequestData&& requestData, RequestCompletionHandler&& handler)
{
    RefPtr webPageProxy = m_webPageProxy.get();
    if (!webPageProxy) {
        handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::NotAllowedError, emptyString() });
        return;
    }

    if (!requestData.frameInfo) {
        RELEASE_LOG_ERROR(WebAuthn, "No frame info for a Web Authentication request.");
        handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::NotAllowedError, "The origin of the document could not be determined."_s });
        return;
    }

    auto relyingPartyIdentifier = relyingPartyIdentifierForRequest(requestData);
    auto callerOrigin = requestData.frameInfo->securityOrigin;

    std::optional<SecurityOriginData> fallbackOrigin;
    if (auto* requestOptions = std::get_if<PublicKeyCredentialRequestOptions>(&requestData.options)) {
        if (requestOptions->extensions && !requestOptions->extensions->crossDeviceFallbackURL.isNull()) {
            if (!protect(webPageProxy->preferences())->webAuthenticationCrossDeviceFallbackURLEnabled())
                requestOptions->extensions->crossDeviceFallbackURL = nullString();
            else {
                URL fallbackURL { requestOptions->extensions->crossDeviceFallbackURL };
                if (auto error = validateCrossDeviceFallbackURL(fallbackURL)) {
                    RELEASE_LOG_ERROR(WebAuthn, "%" PUBLIC_LOG_STRING, error->characters());
                    handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::SecurityError, String { *error } });
                    return;
                }
                fallbackOrigin = SecurityOriginData::fromURL(fallbackURL);
            }
        }
    }

    Vector<SecurityOriginData> originsToValidate;
    if (!relyingPartyIdentifier.isEmpty() && !callerOrigin.securityOrigin()->isMatchingRegistrableDomainSuffix(relyingPartyIdentifier))
        originsToValidate.append(callerOrigin);
    if (fallbackOrigin && !fallbackOrigin->securityOrigin()->isMatchingRegistrableDomainSuffix(relyingPartyIdentifier))
        originsToValidate.append(*fallbackOrigin);

    if (originsToValidate.isEmpty()) {
        performRequestWithValidatedRelyingPartyIdentifier(WTF::move(requestData), WTF::move(handler));
        return;
    }

    bool isValidatingFallbackOrigin = !!fallbackOrigin;
    RelatedOriginsValidation::validate(*webPageProxy, WTF::move(originsToValidate), relyingPartyIdentifier, [weakThis = WeakPtr { *this }, requestData = WTF::move(requestData), handler = WTF::move(handler), isValidatingFallbackOrigin](RelatedOriginsValidation::Result&& result) mutable {
        if (!result.isRelated) {
            auto message = isValidatingFallbackOrigin ? "The origin of the document and the cross-device fallback URL must both be authorized for the provided RP ID."_s : "The origin of the document is not authorized for the provided RP ID."_s;
            RELEASE_LOG_ERROR(WebAuthn, "%" PUBLIC_LOG_STRING, message.characters());
            handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::SecurityError, message });
            return;
        }

        RefPtr protectedThis = weakThis.get();
        if (!protectedThis) {
            handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::NotAllowedError, emptyString() });
            return;
        }

        protectedThis->performRequestWithValidatedRelyingPartyIdentifier(WTF::move(requestData), WTF::move(handler));
    });
}

void WebAuthenticatorCoordinatorProxy::performRequestWithValidatedRelyingPartyIdentifier(WebAuthenticationRequestData&& requestData, RequestCompletionHandler&& handler)
{
    RefPtr webPageProxy = m_webPageProxy.get();
    if (!webPageProxy) {
        handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::NotAllowedError, emptyString() });
        return;
    }

    auto scope = requestData.parentOrigin ? WebAuthn::Scope::CrossOrigin : WebAuthn::Scope::SameOrigin;
    auto topOrigin = requestData.parentOrigin ? requestData.parentOrigin->toString() : nullString();
    Ref callerOrigin = requestData.frameInfo->securityOrigin.securityOrigin();
    auto clientDataJSON = WTF::switchOn(requestData.options, [&](const PublicKeyCredentialCreationOptions& options) {
        return buildClientDataJson(ClientDataType::Create, options.challenge, callerOrigin, scope, topOrigin);
    }, [&](const PublicKeyCredentialRequestOptions& options) {
        return buildClientDataJson(ClientDataType::Get, options.challenge, callerOrigin, scope, topOrigin);
    });
    requestData.hash = buildClientDataJsonHash(clientDataJSON);

    // The manager drives the authenticators its transports find. Where it finds none, or the user
    // gives up, it reports the request as cancelled.
    Ref authenticatorManager = webPageProxy->websiteDataStore().authenticatorManager();
    authenticatorManager->handleRequest(WTF::move(requestData), [handler = WTF::move(handler), clientDataJSON = WTF::move(clientDataJSON)](AuthenticatorManager::Respond&& result) mutable {
        WTF::switchOn(result, [&](const Ref<AuthenticatorResponse>& response) {
            auto responseData = response->data();
            responseData.clientDataJSON = WTF::move(clientDataJSON);
            handler(responseData, response->attachment(), { });
        }, [&](const ExceptionData& exception) {
#if OS(DARWIN)
            // Safari does not tell a page why a ceremony ended without a credential, a dismissal
            // included: the page gets the error with its standard description.
            if (exception.code == ExceptionCode::NotAllowedError) {
                handler({ }, AuthenticatorAttachment::Platform, ExceptionData { ExceptionCode::NotAllowedError, emptyString() });
                return;
            }
#endif
            handler({ }, AuthenticatorAttachment::Platform, exception);
        });
    });
}

void WebAuthenticatorCoordinatorProxy::getClientCapabilities(IPC::Connection&, IPC::Untrusted<SecurityOriginData>&&, CapabilitiesCompletionHandler&& handler)
{
#if OS(DARWIN)
    // On a Mac, WebKit reports what the system's passkey support is able to do, whichever
    // authenticators are within reach at the moment, and Safari's pages count on these answers.
    static constexpr bool passkeySupport = true;
#else
    static constexpr bool passkeySupport = false;
#endif

    // In the order the page enumerates them, which is by key.
    Vector<KeyValuePair<String, bool>> capabilities {
        { "conditionalCreate"_s, passkeySupport },
        { "conditionalGet"_s, passkeySupport },
        { "conditionalMediation"_s, passkeySupport },
        { "extension:largeBlob"_s, passkeySupport },
        { "extension:prf"_s, passkeySupport },
        { "hybridTransport"_s, passkeySupport },
        { "passkeyPlatformAuthenticator"_s, passkeySupport },
        { "relatedOrigins"_s, true },
        { "signalAllAcceptedCredentials"_s, passkeySupport },
        { "signalCurrentUserDetails"_s, passkeySupport },
        { "signalUnknownCredential"_s, passkeySupport },
        { "userVerifyingPlatformAuthenticator"_s, passkeySupport },
    };
    handler(WTF::move(capabilities));
}

// The signal methods update the credentials a credential store keeps for the relying party. The
// port has no such store, and says so the way the Cocoa ports do when theirs does not take a signal.

void WebAuthenticatorCoordinatorProxy::signalUnknownCredential(IPC::Connection& connection, IPC::Untrusted<SecurityOriginData>&& untrustedOrigin, UnknownCredentialOptions&& options, CompletionHandler<void(std::optional<ExceptionData>)>&& completionHandler)
{
    EXTRACT_WITH_MESSAGE_CHECK_COMPLETION(origin, untrustedOrigin,
        completionHandler(ExceptionData { ExceptionCode::SecurityError, "Not authorized for this relying party"_s }), ProcessSpeaksForDomain { connection });
    MESSAGE_CHECK_COMPLETION_BASE(RegistrableDomain::uncheckedCreateFromHost(options.rpId) == RegistrableDomain { origin }, connection,
        completionHandler(ExceptionData { ExceptionCode::SecurityError, "Not authorized for this relying party"_s }));
    if (!base64URLDecode(options.credentialId)) {
        completionHandler(ExceptionData { ExceptionCode::UnknownError, "Unable to parse credential ID."_s });
        return;
    }

    completionHandler(ExceptionData { ExceptionCode::UnknownError, "Error signaling unknown credential."_s });
}

void WebAuthenticatorCoordinatorProxy::signalAllAcceptedCredentials(IPC::Connection& connection, IPC::Untrusted<SecurityOriginData>&& untrustedOrigin, AllAcceptedCredentialsOptions&& options, CompletionHandler<void(std::optional<ExceptionData>)>&& completionHandler)
{
    EXTRACT_WITH_MESSAGE_CHECK_COMPLETION(origin, untrustedOrigin,
        completionHandler(ExceptionData { ExceptionCode::SecurityError, "Not authorized for this relying party"_s }), ProcessSpeaksForDomain { connection });
    MESSAGE_CHECK_COMPLETION_BASE(RegistrableDomain::uncheckedCreateFromHost(options.rpId) == RegistrableDomain { origin }, connection,
        completionHandler(ExceptionData { ExceptionCode::SecurityError, "Not authorized for this relying party"_s }));
    bool parsed = !!base64URLDecode(options.userId);
    for (auto& credentialId : options.allAcceptedCredentialIds)
        parsed = parsed && base64URLDecode(credentialId);
    if (!parsed) {
        completionHandler(ExceptionData { ExceptionCode::UnknownError, "Unable to parse credential ID."_s });
        return;
    }

    completionHandler(ExceptionData { ExceptionCode::UnknownError, "Error signaling all accepted credentials"_s });
}

void WebAuthenticatorCoordinatorProxy::signalCurrentUserDetails(IPC::Connection& connection, IPC::Untrusted<SecurityOriginData>&& untrustedOrigin, CurrentUserDetailsOptions&& options, CompletionHandler<void(std::optional<ExceptionData>)>&& completionHandler)
{
    EXTRACT_WITH_MESSAGE_CHECK_COMPLETION(origin, untrustedOrigin,
        completionHandler(ExceptionData { ExceptionCode::SecurityError, "Not authorized for this relying party"_s }), ProcessSpeaksForDomain { connection });
    MESSAGE_CHECK_COMPLETION_BASE(RegistrableDomain::uncheckedCreateFromHost(options.rpId) == RegistrableDomain { origin }, connection,
        completionHandler(ExceptionData { ExceptionCode::SecurityError, "Not authorized for this relying party"_s }));
    if (!base64URLDecode(options.userId)) {
        completionHandler(ExceptionData { ExceptionCode::UnknownError, "Unable to parse credential ID."_s });
        return;
    }

    completionHandler(ExceptionData { ExceptionCode::UnknownError, "Error signaling current user details."_s });
}

} // namespace WebKit

#undef EXTRACT_WITH_MESSAGE_CHECK_COMPLETION

#endif // ENABLE(WEB_AUTHN)
