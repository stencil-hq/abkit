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

#include <WebCore/FloatRect.h>
#include <WebCore/InteractionQuery.h>
#include <WebCore/Timer.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Forward.h>
#include <wtf/JSONValues.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/WeakPtr.h>

namespace WebCore {
class Page;
}

namespace WebKit {

class WebPage;

// Serves the native DOM read protocol for one page (webkit_web_view_run_interaction_query()):
// parses a JSON request, answers it from WebCore::InteractionQuery, and keeps the requests that
// take time (`resolve` waiting for a box to stop moving, `wait` waiting for a condition).
class InteractionQueryController {
    WTF_MAKE_TZONE_ALLOCATED(InteractionQueryController);
    WTF_MAKE_NONCOPYABLE(InteractionQueryController);
public:
    using Reply = CompletionHandler<void(const String&)>;

    explicit InteractionQueryController(WebPage&);
    ~InteractionQueryController();

    void run(const String& request, Reply&&);

private:
    struct PendingResolve {
        String ref;
        WebCore::InteractionQuery::Action action { WebCore::InteractionQuery::Action::Click };
        unsigned stableFrames { 0 };
        unsigned framesUnchanged { 0 };
        std::optional<WebCore::FloatRect> lastRect;
        MonotonicTime deadline;
        Reply reply;
    };

    enum class WaitState : uint8_t { Visible, Attached, Hidden, Detached };

    struct PendingWait {
        WebCore::InteractionQuery::Query query;
        WaitState state { WaitState::Visible };
        MonotonicTime deadline;
        Reply reply;
    };

    RefPtr<WebCore::Page> corePage() const;

    String snapshot(WebCore::Page&, const JSON::Object& request);
    String query(WebCore::Page&, const JSON::Object& request);
    void resolve(const JSON::Object& request, Reply&&);
    void wait(const JSON::Object& request, Reply&&);

    // Both answer the request and return true once it can be answered.
    bool advance(PendingResolve&);
    bool advance(PendingWait&);

    void startTimerIfNeeded();
    void frameTimerFired();

    WeakPtr<WebPage> m_webPage;
    Vector<PendingResolve> m_resolves;
    Vector<PendingWait> m_waits;
    WebCore::Timer m_frameTimer;
    unsigned m_framesSinceWaitsChecked { 0 };
};

} // namespace WebKit
