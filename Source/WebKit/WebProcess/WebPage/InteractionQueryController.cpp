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
#include "InteractionQueryController.h"

#include "WebPage.h"
#include <WebCore/Element.h>
#include <WebCore/Page.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
using namespace WebCore;
using namespace WebCore::InteractionQuery;

WTF_MAKE_TZONE_ALLOCATED_IMPL(InteractionQueryController);

// One display frame at 60 Hz: how often a box is sampled to decide that it has stopped moving.
static constexpr auto frameInterval = 16_ms;
// Conditions are re-evaluated every few frames; a whole-document query per frame would be wasteful.
static constexpr unsigned framesBetweenWaitChecks = 3;

static constexpr unsigned defaultSnapshotLimit = 2000;
static constexpr unsigned defaultQueryLimit = 100;
static constexpr unsigned defaultStableFrames = 2;
static constexpr auto defaultResolveTimeout = 5_s;
static constexpr auto defaultWaitTimeout = 30_s;

static String errorReply(ASCIILiteral kind, const String& message)
{
    auto error = JSON::Object::create();
    error->setString("kind"_s, kind);
    error->setString("message"_s, message);

    auto reply = JSON::Object::create();
    reply->setObject("error"_s, WTF::move(error));
    return reply->toJSONString();
}

static Ref<JSON::Array> pointAsJSON(FloatPoint point)
{
    auto array = JSON::Array::create();
    array->pushDouble(point.x());
    array->pushDouble(point.y());
    return array;
}

static Ref<JSON::Array> rectAsJSON(const FloatRect& rect)
{
    auto array = JSON::Array::create();
    array->pushDouble(rect.x());
    array->pushDouble(rect.y());
    array->pushDouble(rect.width());
    array->pushDouble(rect.height());
    return array;
}

static unsigned countOr(const JSON::Object& request, ASCIILiteral key, unsigned fallback)
{
    auto value = request.getInteger(key);
    return value && *value >= 0 ? static_cast<unsigned>(*value) : fallback;
}

static MonotonicTime deadlineFor(const JSON::Object& request, Seconds fallback)
{
    auto timeout = request.getDouble("timeout"_s);
    return MonotonicTime::now() + (timeout && *timeout >= 0 ? Seconds::fromMilliseconds(*timeout) : fallback);
}

// A root that was named but no longer exists is an error rather than a silent search of the whole page.
static std::expected<RefPtr<Element>, String> rootFor(Page& page, const JSON::Object& request)
{
    auto ref = request.getString("root"_s);
    if (ref.isNull())
        return RefPtr<Element> { };

    RefPtr root = elementForRef(page, ref);
    if (!root)
        return makeUnexpected(makeString("root is detached: "_s, ref));
    return root;
}

static std::expected<Query, String> parseQuery(Page& page, const JSON::Object& request)
{
    static constexpr std::array kinds {
        std::pair { "css"_s, QueryKind::CSS },
        std::pair { "xpath"_s, QueryKind::XPath },
        std::pair { "text"_s, QueryKind::Text },
        std::pair { "role"_s, QueryKind::Role },
        std::pair { "label"_s, QueryKind::Label },
        std::pair { "attribute"_s, QueryKind::Attribute },
        std::pair { "ref"_s, QueryKind::Ref },
    };

    auto by = request.getString("by"_s);
    auto kind = std::ranges::find_if(kinds, [&](auto& candidate) {
        return by == candidate.first;
    });
    if (kind == kinds.end())
        return makeUnexpected(makeString("unknown query kind: "_s, by));

    auto root = rootFor(page, request);
    if (!root)
        return makeUnexpected(root.error());

    Query query;
    query.kind = kind->second;
    query.value = request.getString("value"_s);
    query.name = request.getString("name"_s);
    query.attribute = request.getString("attribute"_s);
    query.exact = request.getBoolean("exact"_s).value_or(false);
    query.visibleOnly = request.getBoolean("visibleOnly"_s).value_or(false);
    query.root = WTF::move(*root);
    return query;
}

static OptionSet<Include> parseInclude(const JSON::Object& request)
{
    OptionSet<Include> include;
    RefPtr names = request.getArray("include"_s);
    if (!names)
        return include;

    for (auto& value : *names) {
        auto name = value->asString();
        if (name == "text"_s)
            include.add(Include::Text);
        else if (name == "html"_s)
            include.add(Include::HTML);
        else if (name == "value"_s)
            include.add(Include::Value);
        else if (name == "attributes"_s)
            include.add(Include::Attributes);
        else if (name == "checked"_s)
            include.add(Include::Checked);
    }
    return include;
}

static Ref<JSON::Array> describeAll(const Vector<Ref<Element>>& elements, unsigned limit, OptionSet<Include> include)
{
    auto nodes = JSON::Array::create();
    for (auto& element : elements.span().first(std::min<size_t>(elements.size(), limit)))
        nodes->pushObject(describe(element, include));
    return nodes;
}

static ASCIILiteral stateName(TargetState state)
{
    switch (state) {
    case TargetState::Ok:
        return "ok"_s;
    case TargetState::Hidden:
        return "hidden"_s;
    case TargetState::Disabled:
        return "disabled"_s;
    case TargetState::NotEditable:
        return "notEditable"_s;
    case TargetState::OffViewport:
        return "offViewport"_s;
    case TargetState::Covered:
        return "covered"_s;
    }
    return "hidden"_s;
}

static String targetReply(Element& element, const Target& target, ASCIILiteral state)
{
    auto reply = JSON::Object::create();
    reply->setString("state"_s, state);
    reply->setObject("node"_s, describe(element));

    if (target.state == TargetState::Ok || target.state == TargetState::Covered) {
        reply->setArray("point"_s, pointAsJSON(target.point));
        reply->setArray("rect"_s, rectAsJSON(target.rect));
    }
    if (target.scroll) {
        auto scroll = JSON::Object::create();
        scroll->setArray("at"_s, pointAsJSON(target.scroll->at));
        scroll->setDouble("dx"_s, target.scroll->delta.width());
        scroll->setDouble("dy"_s, target.scroll->delta.height());
        reply->setObject("scroll"_s, WTF::move(scroll));
    }
    if (RefPtr coveredBy = target.coveredBy)
        reply->setObject("coveredBy"_s, describe(*coveredBy));
    return reply->toJSONString();
}

InteractionQueryController::InteractionQueryController(WebPage& webPage)
    : m_webPage(webPage)
    , m_frameTimer([this] {
        frameTimerFired();
    })
{
}

InteractionQueryController::~InteractionQueryController()
{
    auto closed = errorReply("failed"_s, "the page was closed"_s);
    for (auto& pending : std::exchange(m_resolves, { }))
        pending.reply(closed);
    for (auto& pending : std::exchange(m_waits, { }))
        pending.reply(closed);
}

RefPtr<Page> InteractionQueryController::corePage() const
{
    RefPtr webPage = m_webPage.get();
    return webPage ? webPage->corePage() : nullptr;
}

void InteractionQueryController::run(const String& requestJSON, Reply&& reply)
{
    RefPtr page = corePage();
    RefPtr value = JSON::Value::parseJSON(requestJSON);
    RefPtr request = value ? value->asObject() : nullptr;
    if (!page || !request)
        return reply(errorReply("invalid"_s, "the request is not a JSON object"_s));

    ReadScope scope(*page);

    auto read = request->getString("read"_s);
    if (read == "snapshot"_s)
        return reply(snapshot(*page, *request));
    if (read == "query"_s)
        return reply(query(*page, *request));
    if (read == "resolve"_s)
        return resolve(*request, WTF::move(reply));
    if (read == "wait"_s)
        return wait(*request, WTF::move(reply));

    reply(errorReply("invalid"_s, makeString("unknown read: "_s, read)));
}

String InteractionQueryController::snapshot(Page& page, const JSON::Object& request)
{
    auto root = rootFor(page, request);
    if (!root)
        return errorReply("invalid"_s, root.error());

    InteractionQuery::SnapshotOptions options;
    options.root = WTF::move(*root);
    options.includeAll = request.getBoolean("includeAll"_s).value_or(false);
    options.viewportOnly = request.getBoolean("viewportOnly"_s).value_or(false);
    options.limit = countOr(request, "limit"_s, defaultSnapshotLimit);
    return InteractionQuery::snapshot(page, options)->toJSONString();
}

String InteractionQueryController::query(Page& page, const JSON::Object& request)
{
    auto query = parseQuery(page, request);
    if (!query)
        return errorReply("invalid"_s, query.error());

    auto matches = InteractionQuery::query(page, *query);
    if (!matches)
        return errorReply("invalid"_s, matches.error());

    auto reply = JSON::Object::create();
    reply->setArray("nodes"_s, describeAll(*matches, countOr(request, "limit"_s, defaultQueryLimit), parseInclude(request)));
    reply->setInteger("total"_s, static_cast<int>(matches->size()));
    return reply->toJSONString();
}

void InteractionQueryController::resolve(const JSON::Object& request, Reply&& reply)
{
    PendingResolve pending;
    pending.ref = request.getString("ref"_s);

    auto action = request.getString("action"_s);
    if (action.isNull() || action == "click"_s)
        pending.action = Action::Click;
    else if (action == "hover"_s)
        pending.action = Action::Hover;
    else if (action == "type"_s)
        pending.action = Action::Type;
    else if (action == "none"_s)
        pending.action = Action::None;
    else
        return reply(errorReply("invalid"_s, makeString("unknown action: "_s, action)));

    pending.stableFrames = countOr(request, "stable"_s, defaultStableFrames);
    pending.deadline = deadlineFor(request, defaultResolveTimeout);
    pending.reply = WTF::move(reply);

    if (advance(pending))
        return;
    m_resolves.append(WTF::move(pending));
    startTimerIfNeeded();
}

void InteractionQueryController::wait(const JSON::Object& request, Reply&& reply)
{
    RefPtr condition = request.getObject("for"_s);
    if (!condition)
        return reply(errorReply("invalid"_s, "a wait needs a \"for\" query"_s));

    RefPtr page = corePage();
    if (!page)
        return reply(errorReply("failed"_s, "the page was closed"_s));

    auto query = parseQuery(*page, *condition);
    if (!query)
        return reply(errorReply("invalid"_s, query.error()));

    PendingWait pending;
    pending.query = WTF::move(*query);

    auto state = request.getString("state"_s);
    if (state.isNull() || state == "visible"_s)
        pending.state = WaitState::Visible;
    else if (state == "attached"_s)
        pending.state = WaitState::Attached;
    else if (state == "hidden"_s)
        pending.state = WaitState::Hidden;
    else if (state == "detached"_s)
        pending.state = WaitState::Detached;
    else
        return reply(errorReply("invalid"_s, makeString("unknown wait state: "_s, state)));

    pending.deadline = deadlineFor(request, defaultWaitTimeout);
    pending.reply = WTF::move(reply);

    if (advance(pending))
        return;
    m_waits.append(WTF::move(pending));
    startTimerIfNeeded();
}

bool InteractionQueryController::advance(PendingResolve& pending)
{
    RefPtr page = corePage();
    RefPtr element = page ? elementForRef(*page, pending.ref) : nullptr;
    if (!element) {
        auto reply = JSON::Object::create();
        reply->setString("state"_s, "detached"_s);
        pending.reply(reply->toJSONString());
        return true;
    }

    auto target = InteractionQuery::resolve(*element, pending.action);

    // Only an answer that depends on where the box is right now has to wait for the box to settle.
    bool dependsOnBox = target.state == TargetState::Ok || target.state == TargetState::Covered;
    if (dependsOnBox && pending.stableFrames) {
        pending.framesUnchanged = pending.lastRect == target.rect ? pending.framesUnchanged + 1 : 0;
        pending.lastRect = target.rect;
        if (pending.framesUnchanged < pending.stableFrames) {
            if (MonotonicTime::now() < pending.deadline)
                return false;
            pending.reply(targetReply(*element, target, "unstable"_s));
            return true;
        }
    }

    pending.reply(targetReply(*element, target, stateName(target.state)));
    return true;
}

bool InteractionQueryController::advance(PendingWait& pending)
{
    RefPtr page = corePage();
    if (!page) {
        pending.reply(errorReply("failed"_s, "the page was closed"_s));
        return true;
    }

    // A scope that has left the document can no longer contain anything.
    Vector<Ref<Element>> matches;
    if (!pending.query.root || pending.query.root->isConnected()) {
        auto result = InteractionQuery::query(*page, pending.query);
        if (!result) {
            pending.reply(errorReply("invalid"_s, result.error()));
            return true;
        }
        matches = WTF::move(*result);
    }

    bool wantsVisibility = pending.state == WaitState::Visible || pending.state == WaitState::Hidden;
    if (wantsVisibility) {
        matches.removeAllMatching([](auto& element) {
            return !isVisible(element);
        });
    }

    bool wantsMatches = pending.state == WaitState::Visible || pending.state == WaitState::Attached;
    bool satisfied = wantsMatches != matches.isEmpty();
    if (!satisfied && MonotonicTime::now() < pending.deadline)
        return false;

    auto reply = JSON::Object::create();
    reply->setBoolean("matched"_s, satisfied);
    if (satisfied && wantsMatches)
        reply->setArray("nodes"_s, describeAll(matches, defaultQueryLimit, { }));
    pending.reply(reply->toJSONString());
    return true;
}

void InteractionQueryController::startTimerIfNeeded()
{
    if (!m_frameTimer.isActive())
        m_frameTimer.startRepeating(frameInterval);
}

void InteractionQueryController::frameTimerFired()
{
    RefPtr page = corePage();
    std::optional<ReadScope> scope;
    if (page)
        scope.emplace(*page);

    m_resolves.removeAllMatching([&](auto& pending) {
        return advance(pending);
    });

    bool waitsAreDue = ++m_framesSinceWaitsChecked >= framesBetweenWaitChecks;
    if (waitsAreDue) {
        m_framesSinceWaitsChecked = 0;
        m_waits.removeAllMatching([&](auto& pending) {
            return advance(pending);
        });
    }

    if (m_resolves.isEmpty() && m_waits.isEmpty())
        m_frameTimer.stop();
}

} // namespace WebKit
