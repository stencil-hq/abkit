/*
 * Copyright (C) 2025 Shopify Inc. All rights reserved.
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

#include "config.h"
#include "SpeculationRulesMatcher.h"

#include "CheckVisibilityOptions.h"
#include "ComposedTreeIterator.h"
#include "Document.h"
#include "Element.h"
#include "HTMLAnchorElement.h"
#include "JSDOMGlobalObject.h"
#include "ReferrerPolicy.h"
#include "RenderElement.h"
#include "ScriptController.h"
#include "SelectorQuery.h"
#include "ShadowRoot.h"
#include "SpeculationRules.h"
#include "URLPattern.h"
#include "URLPatternOptions.h"

namespace WebCore {

NODELETE static bool isUnslottedElement(Element& element)
{
    for (auto* ancestor = &element; ancestor; ) {
        auto* parent = ancestor->parentElement();
        if (parent && parent->shadowRoot() && !ancestor->assignedSlot())
            return true;
        ancestor = parent;
    }
    return false;
}

static bool hasRenderedDescendants(Element& element)
{
    for (CheckedRef descendant : composedTreeDescendants(element)) {
        if (descendant->renderer())
            return true;
    }
    return false;
}

static bool matches(const SpeculationRules::DocumentPredicate&, SpeculationRules&, HTMLAnchorElement&, const URL&);

static bool matches(const SpeculationRules::URLPatternPredicate& predicate, SpeculationRules& rules, HTMLAnchorElement& anchor, const URL& url)
{
    if (!url.isValid())
        return false;

    const auto& baseURL = anchor.document().baseURL();
    return std::ranges::any_of(predicate.patterns, [&](const auto& pattern) {
        auto* compiled = rules.urlPattern(pattern, baseURL);
        return compiled && compiled->test(url);
    });
}

static bool matches(const SpeculationRules::CSSSelectorPredicate& predicate, Element& element)
{
    for (const auto& selectorString : predicate.selectors) {
        auto query = protect(element.document())->selectorQueryForString(selectorString);
        if (query.hasException())
            continue;
        if (query.returnValue().matches(element))
            return true;
    }
    return false;
}

static bool matches(const Box<SpeculationRules::Conjunction>& predicate, SpeculationRules& rules, HTMLAnchorElement& anchor, const URL& url)
{
    for (const auto& clause : predicate->clauses) {
        if (!matches(clause, rules, anchor, url))
            return false;
    }
    return true;
}

static bool matches(const Box<SpeculationRules::Disjunction>& predicate, SpeculationRules& rules, HTMLAnchorElement& anchor, const URL& url)
{
    if (predicate->clauses.isEmpty())
        return false;

    for (const auto& clause : predicate->clauses) {
        if (matches(clause, rules, anchor, url))
            return true;
    }
    return false;
}

static bool matches(const Box<SpeculationRules::Negation>& predicate, SpeculationRules& rules, HTMLAnchorElement& anchor, const URL& url)
{
    return !matches(*predicate->clause, rules, anchor, url);
}

// https://html.spec.whatwg.org/C#dr-predicate-matches
static bool matches(const SpeculationRules::DocumentPredicate& predicate, SpeculationRules& rules, HTMLAnchorElement& anchor, const URL& url)
{
    return WTF::switchOn(predicate.value(),
        [&] (const SpeculationRules::URLPatternPredicate& p) { return matches(p, rules, anchor, url); },
        [&] (const SpeculationRules::CSSSelectorPredicate& p) { return matches(p, anchor); },
        [&] (const Box<SpeculationRules::Conjunction>& p) { return matches(p, rules, anchor, url); },
        [&] (const Box<SpeculationRules::Disjunction>& p) { return matches(p, rules, anchor, url); },
        [&] (const Box<SpeculationRules::Negation>& p) { return matches(p, rules, anchor, url); }
    );
}

// https://html.spec.whatwg.org/C#find-matching-links
std::optional<PrefetchRule> SpeculationRulesMatcher::hasMatchingRule(Document& document, HTMLAnchorElement& anchor)
{
    Ref speculationRules = document.speculationRules();
    if (speculationRules->prefetchRules().computeIsEmpty())
        return std::nullopt;

    // 2.2 If descendant is not being rendered or is part of skipped contents, then continue.
    // An element is not being rendered if:
    // - It's unslotted (light DOM child of a shadow host without a slot assignment)
    // - It or an ancestor has display:none
    // - It's part of content-visibility:hidden content
    if (isUnslottedElement(anchor))
        return std::nullopt;

    if (!anchor.checkVisibility(CheckVisibilityOptions { })) {
        // checkVisibility returns false for elements with no renderer, which includes both
        // display:none and display:contents.
        //
        // `display: none` elements can't have rendered descendants.
        if (!anchor.hasDisplayContents())
            return std::nullopt;
        // `display: contents` elements can have rendered descendants, so we need to check for them.
        if (!hasRenderedDescendants(anchor))
            return std::nullopt;
    }

    const auto& url = anchor.href();

    for (auto [node, rules] : speculationRules->prefetchRules()) {
        for (const auto& rule : rules) {
            for (const auto& href : rule.urls) {
                if (href == url)
                    return PrefetchRule { rule.tags, rule.referrerPolicy, rule.eagerness };
            }

            if (rule.predicate && matches(rule.predicate.value(), speculationRules, anchor, url)) {
                // For document rules, if the rule doesn't specify a referrer policy,
                // use the link element's referrerPolicy attribute if present.
                auto referrerPolicy = rule.referrerPolicy;
                if (!referrerPolicy) {
                    auto linkReferrerPolicy = anchor.referrerPolicy();
                    if (linkReferrerPolicy != ReferrerPolicy::EmptyString)
                        referrerPolicy = linkReferrerPolicy;
                }
                return PrefetchRule { rule.tags, referrerPolicy, rule.eagerness };
            }
        }
    }

    return std::nullopt;
}

} // namespace WebCore
