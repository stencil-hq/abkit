/*
 * Copyright (C) 2016 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "NetworkSession.h"
#include "SoupCookiePersistentStorageType.h"
#include "WebPageProxyIdentifier.h"
#include <wtf/Function.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/CString.h>

typedef struct _SoupSession SoupSession;

namespace WebCore {
class CertificateInfo;
class SoupNetworkSession;
struct SoupNetworkProxySettings;
}

namespace WebKit {

class DataTaskLoad;
class NetworkSocketChannel;
class WebSocketTask;
struct NetworkSessionCreationParameters;

class NetworkSessionSoup final : public NetworkSession {
    WTF_MAKE_TZONE_ALLOCATED(NetworkSessionSoup);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(NetworkSessionSoup);
public:
    static std::unique_ptr<NetworkSession> create(NetworkProcess& networkProcess, const NetworkSessionCreationParameters& parameters)
    {
        return makeUnique<NetworkSessionSoup>(networkProcess, parameters);
    }
    NetworkSessionSoup(NetworkProcess&, const NetworkSessionCreationParameters&);
    ~NetworkSessionSoup();

    WebCore::SoupNetworkSession& soupNetworkSession() const { return *m_networkSession; }
    SoupSession* soupSession() const;
    // The soup session a request with the given stored-credentials policy is sent on. On Darwin a
    // request that does not use stored credentials goes on a separate session, so that it can never
    // share a connection with one that does, as CFNetwork does on the Cocoa ports.
    SoupSession* soupSessionForStoredCredentialsPolicy(WebCore::StoredCredentialsPolicy) const;
    WebCore::SoupNetworkSession& soupNetworkSessionForStoredCredentialsPolicy(WebCore::StoredCredentialsPolicy) const;

    void setCookiePersistentStorage(const String& storagePath, SoupCookiePersistentStorageType);
    void setAcceptLanguages(const UTF8CString&) const;

    void setPersistentCredentialStorageEnabled(bool enabled) { m_persistentCredentialStorageEnabled = enabled; }
    bool persistentCredentialStorageEnabled() const { return m_persistentCredentialStorageEnabled; }

    void setIgnoreTLSErrors(bool);
    void allowSpecificHTTPSCertificateForHost(const WebCore::CertificateInfo&, const String&);
    void setProxySettings(const WebCore::SoupNetworkProxySettings&);

private:
    RefPtr<WebSocketTask> createWebSocketTask(WebPageProxyIdentifier, std::optional<WebCore::FrameIdentifier>, std::optional<WebCore::PageIdentifier>, NetworkSocketChannel&, const WebCore::ResourceRequest&, const String& protocol, const WebCore::ClientOrigin&, bool, bool, OptionSet<WebCore::AdvancedPrivacyProtections>, WebCore::StoredCredentialsPolicy, WebCore::IsInitiatedByWorker) final;
    void clearCredentials(WallTime) final;
    void dataTaskWithRequest(WebPageProxyIdentifier, WebCore::ResourceRequest&&, const std::optional<WebCore::SecurityOriginData>& topOrigin, CompletionHandler<void(DataTaskIdentifier)>&&) final;
    void cancelDataTask(DataTaskIdentifier) final;

    void forEachSoupNetworkSession(NOESCAPE const Function<void(WebCore::SoupNetworkSession&)>&) const;

    const std::unique_ptr<WebCore::SoupNetworkSession> m_networkSession;
#if OS(DARWIN)
    // Sessions that do not carry cookies or credentials and pool their own connections.
    const std::unique_ptr<WebCore::SoupNetworkSession> m_networkSessionWithoutCredentials;
#endif
    bool m_persistentCredentialStorageEnabled { true };
    HashMap<DataTaskIdentifier, Ref<DataTaskLoad>> m_dataTaskLoads;
};

} // namespace WebKit
