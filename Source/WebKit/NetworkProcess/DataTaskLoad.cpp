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
#include "DataTaskLoad.h"

#include "AuthenticationChallengeDisposition.h"
#include "Connection.h"
#include "NetworkDataTaskBlob.h"
#include "NetworkLoadParameters.h"
#include "NetworkProcess.h"
#include "NetworkProcessProxyMessages.h"
#include "NetworkSession.h"
#include "WebErrors.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/Credential.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(DataTaskLoad);

Ref<DataTaskLoad> DataTaskLoad::create(NetworkSession& session, DataTaskIdentifier identifier, WebPageProxyIdentifier pageID, ResourceRequest&& request, const std::optional<SecurityOriginData>& topOrigin, Function<void()>&& didFinish)
{
    return adoptRef(*new DataTaskLoad(session, identifier, pageID, WTF::move(request), topOrigin, WTF::move(didFinish)));
}

DataTaskLoad::DataTaskLoad(NetworkSession& session, DataTaskIdentifier identifier, WebPageProxyIdentifier pageID, ResourceRequest&& request, const std::optional<SecurityOriginData>& topOrigin, Function<void()>&& didFinish)
    : m_connection(session.networkProcess().parentProcessConnection())
    , m_identifier(identifier)
    , m_request(WTF::move(request))
    , m_didFinish(WTF::move(didFinish))
{
    NetworkLoadParameters parameters;
    parameters.webPageProxyID = pageID;
    parameters.request = m_request;
    parameters.storedCredentialsPolicy = StoredCredentialsPolicy::Use;
    if (topOrigin)
        parameters.topOrigin = topOrigin->securityOrigin();

    if (m_request.url().protocolIsBlob())
        m_task = NetworkDataTaskBlob::create(session, *this, m_request, parameters.blobFileReferences, parameters.topOrigin);
    else
        m_task = NetworkDataTask::create(session, *this, parameters);
}

DataTaskLoad::~DataTaskLoad()
{
    stop();
}

void DataTaskLoad::start()
{
    if (RefPtr task = m_task)
        task->resume();
}

void DataTaskLoad::cancel()
{
    // The UI process forgets a task when it cancels it, so nothing more is reported.
    m_didFinish = nullptr;
    stop();
}

void DataTaskLoad::stop()
{
    if (RefPtr task = std::exchange(m_task, nullptr)) {
        task->clearClient();
        task->cancel();
    }
}

void DataTaskLoad::complete(const ResourceError& error)
{
    auto didFinish = std::exchange(m_didFinish, nullptr);
    if (!didFinish)
        return;

    if (RefPtr connection = m_connection)
        connection->send(Messages::NetworkProcessProxy::DataTaskDidCompleteWithError(m_identifier, error), 0);
    didFinish();
}

void DataTaskLoad::willPerformHTTPRedirection(ResourceResponse&& response, ResourceRequest&& request, RedirectCompletionHandler&& completionHandler)
{
    RefPtr connection = m_connection;
    if (!connection)
        return completionHandler({ });

    connection->sendWithAsyncReply(Messages::NetworkProcessProxy::DataTaskWillPerformHTTPRedirection(m_identifier, response, request), [protectedThis = Ref { *this }, newRequest = request, completionHandler = WTF::move(completionHandler)](bool allowed) mutable {
        if (allowed)
            return completionHandler(WTF::move(newRequest));

        // A refused redirect ends the load.
        completionHandler({ });
        protectedThis->stop();
        protectedThis->complete(cancelledError(protectedThis->m_request));
    }, 0);
}

void DataTaskLoad::didReceiveChallenge(AuthenticationChallenge&& challenge, NegotiatedLegacyTLS, ChallengeCompletionHandler&& completionHandler)
{
    RefPtr connection = m_connection;
    if (!connection)
        return completionHandler(AuthenticationChallengeDisposition::Cancel, { });

    connection->sendWithAsyncReply(Messages::NetworkProcessProxy::DataTaskReceivedChallenge(m_identifier, challenge), [completionHandler = WTF::move(completionHandler)](AuthenticationChallengeDisposition disposition, Credential&& credential) mutable {
        completionHandler(disposition, credential);
    }, 0);
}

void DataTaskLoad::didReceiveResponse(ResourceResponse&& response, NegotiatedLegacyTLS, PrivateRelayed, ResponseCompletionHandler&& completionHandler)
{
    RefPtr connection = m_connection;
    if (!connection)
        return completionHandler(PolicyAction::Ignore);

    connection->sendWithAsyncReply(Messages::NetworkProcessProxy::DataTaskDidReceiveResponse(m_identifier, response), [protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)](bool allowed) mutable {
        if (allowed)
            return completionHandler(PolicyAction::Use);

        // A refused response ends the load.
        completionHandler(PolicyAction::Ignore);
        protectedThis->stop();
        protectedThis->complete(cancelledError(protectedThis->m_request));
    }, 0);
}

void DataTaskLoad::didReceiveData(const SharedBuffer& buffer)
{
    if (RefPtr connection = m_connection)
        connection->send(Messages::NetworkProcessProxy::DataTaskDidReceiveData(m_identifier, buffer.span()), 0);
}

void DataTaskLoad::didCompleteWithError(const ResourceError& error, const NetworkLoadMetrics&)
{
    Ref protectedThis { *this };
    if (RefPtr task = std::exchange(m_task, nullptr))
        task->clearClient();
    complete(error);
}

void DataTaskLoad::wasBlocked()
{
    Ref protectedThis { *this };
    stop();
    complete(blockedError(m_request));
}

void DataTaskLoad::cannotShowURL()
{
    Ref protectedThis { *this };
    stop();
    complete(cannotShowURLError(m_request));
}

void DataTaskLoad::wasBlockedByRestrictions()
{
    Ref protectedThis { *this };
    stop();
    complete(wasBlockedByRestrictionsError(m_request));
}

void DataTaskLoad::wasBlockedByDisabledFTP()
{
    Ref protectedThis { *this };
    stop();
    complete(ftpDisabledError(m_request));
}

} // namespace WebKit
