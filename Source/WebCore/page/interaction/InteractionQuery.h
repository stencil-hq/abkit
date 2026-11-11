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

#include <WebCore/FloatPoint.h>
#include <WebCore/FloatRect.h>
#include <WebCore/FloatSize.h>
#include <expected>
#include <optional>
#include <wtf/JSONValues.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class Element;
class Page;
enum class AccessibilityMode : uint8_t;

// Read-only views of a page for a host that drives it with real input (pointer, wheel, keys):
// what is on the page, where to aim, and what matches a selector. Nothing here runs script,
// dispatches events or mutates the document, so a page cannot tell it is being read.
// WebKit::InteractionQueryController speaks the JSON protocol built on these functions.
namespace InteractionQuery {

// Brackets a batch of the reads below. Brings style and layout up to date in every frame and gives
// the reads an accessibility tree, the source of roles and names, for as long as the scope lives.
// The tree is this process's alone and goes away with the scope unless an accessibility client had
// already asked for one, so a page carries its cost only while it is being read.
class ReadScope {
    WTF_MAKE_NONCOPYABLE(ReadScope);
public:
    WEBCORE_EXPORT explicit ReadScope(Page&);
    WEBCORE_EXPORT ~ReadScope();

private:
    const Ref<Page> m_page;
    const AccessibilityMode m_previousMode;
};

// A ref names an element for as long as it lives in this process: "e<process>-<node>".
WEBCORE_EXPORT String refForElement(Element&);
// Null when the ref is malformed, was minted by another process or for another page, or its element is
// gone, disconnected, or in a document its frame no longer shows (one kept for going back, say).
WEBCORE_EXPORT RefPtr<Element> elementForRef(Page&, const String&);

enum class Include : uint8_t {
    Text = 1 << 0,
    HTML = 1 << 1,
    Value = 1 << 2,
    Attributes = 1 << 3,
    Checked = 1 << 4,
};

// The protocol's Node object for one element, without children.
WEBCORE_EXPORT Ref<JSON::Object> describe(Element&, OptionSet<Include> = { });

struct SnapshotOptions {
    RefPtr<Element> root;
    bool includeAll { false };
    bool viewportOnly { false };
    unsigned limit { 2000 };
};

// The protocol's snapshot reply: page metrics and the forest of nodes under the root (the main document by default).
WEBCORE_EXPORT Ref<JSON::Object> snapshot(Page&, const SnapshotOptions&);

enum class Action : uint8_t { None, Click, Hover, Type };

enum class TargetState : uint8_t { Ok, Hidden, Disabled, NotEditable, OffViewport, Covered };

// A wheel event that moves a target towards view: dispatch it at `at` (root view coordinates) with `delta`.
struct Scroll {
    FloatPoint at;
    FloatSize delta;
};

struct Target {
    TargetState state { TargetState::Hidden };
    // The visible part of the element and the point to aim at, in root view coordinates.
    FloatRect rect;
    FloatPoint point;
    std::optional<Scroll> scroll;
    RefPtr<Element> coveredBy;
};

// Whether input aimed at the element would reach it right now, and what to do when it would not.
WEBCORE_EXPORT Target resolve(Element&, Action);

// Rendered, not `visibility: hidden`, and not zero-sized. Says nothing about clipping or scroll position.
WEBCORE_EXPORT bool isVisible(Element&);

enum class QueryKind : uint8_t { CSS, XPath, Text, Role, Label, Attribute, Ref };

struct Query {
    QueryKind kind { QueryKind::CSS };
    String value;
    String name;
    String attribute;
    bool exact { false };
    bool visibleOnly { false };
    RefPtr<Element> root;
};

// Matching elements in document order across frames and shadow trees, or why the query is invalid.
WEBCORE_EXPORT std::expected<Vector<Ref<Element>>, String> query(Page&, const Query&);

} // namespace InteractionQuery
} // namespace WebCore
