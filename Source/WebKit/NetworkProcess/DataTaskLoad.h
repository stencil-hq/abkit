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

#include "DataTaskIdentifier.h"
#include "NetworkDataTask.h"
#include "WebPageProxyIdentifier.h"
#include <WebCore/ResourceRequest.h>
#include <wtf/Function.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>

namespace IPC {
class Connection;
}

namespace WebCore {
class SecurityOriginData;
}

namespace WebKit {

class NetworkSession;

// A load the UI process asked for as a data task (API::DataTask). The UI process is told what
// happens to it and decides about its redirects, responses and authentication challenges.
class DataTaskLoad final : public RefCounted<DataTaskLoad>, private NetworkDataTaskClient {
    WTF_MAKE_TZONE_ALLOCATED(DataTaskLoad);
public:
    // didFinish is called once, when the load has reported its completion; not after cancel().
    static Ref<DataTaskLoad> create(NetworkSession&, DataTaskIdentifier, WebPageProxyIdentifier, WebCore::ResourceRequest&&, const std::optional<WebCore::SecurityOriginData>& topOrigin, Function<void()>&& didFinish);
    ~DataTaskLoad();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    void start();
    void cancel();

private:
    DataTaskLoad(NetworkSession&, DataTaskIdentifier, WebPageProxyIdentifier, WebCore::ResourceRequest&&, const std::optional<WebCore::SecurityOriginData>& topOrigin, Function<void()>&& didFinish);

    // NetworkDataTaskClient.
    void willPerformHTTPRedirection(WebCore::ResourceResponse&&, WebCore::ResourceRequest&&, RedirectCompletionHandler&&) final;
    void didReceiveChallenge(WebCore::AuthenticationChallenge&&, NegotiatedLegacyTLS, ChallengeCompletionHandler&&) final;
    void didReceiveResponse(WebCore::ResourceResponse&&, NegotiatedLegacyTLS, PrivateRelayed, ResponseCompletionHandler&&) final;
    void didReceiveData(const WebCore::SharedBuffer&) final;
    void didCompleteWithError(const WebCore::ResourceError&, const WebCore::NetworkLoadMetrics&) final;
    void didSendData(uint64_t, uint64_t) final { }
    void wasBlocked() final;
    void cannotShowURL() final;
    void wasBlockedByRestrictions() final;
    void wasBlockedByDisabledFTP() final;

    void stop();
    void complete(const WebCore::ResourceError&);

    const RefPtr<IPC::Connection> m_connection;
    const DataTaskIdentifier m_identifier;
    const WebCore::ResourceRequest m_request;
    RefPtr<NetworkDataTask> m_task;
    Function<void()> m_didFinish;
};

} // namespace WebKit
