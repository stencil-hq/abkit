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
#include "InteractionQuery.h"

#include "AXObjectCache.h"
#include "AXObjectCacheInlines.h"
#include "AccessibilityObject.h"
#include "CheckVisibilityOptions.h"
#include "ComposedTreeIterator.h"
#include "ContainerNodeInlines.h"
#include "DocumentPage.h"
#include "DocumentView.h"
#include "ElementInlines.h"
#include "EventHandler.h"
#include "EventNames.h"
#include "EventTargetInlines.h"
#include "FloatQuad.h"
#include "HTMLButtonElement.h"
#include "HTMLFrameOwnerElement.h"
#include "HTMLInputElement.h"
#include "HTMLLabelElement.h"
#include "HTMLNames.h"
#include "HTMLOptionElement.h"
#include "HTMLSelectElement.h"
#include "HTMLSummaryElement.h"
#include "HTMLTextAreaElement.h"
#include "HitTestRequest.h"
#include "HitTestResult.h"
#include "LocalFrame.h"
#include "LocalFrameView.h"
#include "NodeInlines.h"
#include "NodeList.h"
#include "Page.h"
#include "ProcessIdentifier.h"
#include "RenderBoxInlines.h"
#include "RenderElementInlines.h"
#include "RenderElementStyleInlines.h"
#include "RenderLayer.h"
#include "RenderLayerScrollableArea.h"
#include "RenderObjectDocument.h"
#include "RenderObjectInlines.h"
#include "RenderText.h"
#include "RenderView.h"
#include "SelectorQuery.h"
#include "Settings.h"
#include "ShadowRoot.h"
#include "SpaceSplitString.h"
#include "Text.h"
#include "TreeScopeInlines.h"
#include "TypedElementDescendantIteratorInlines.h"
#include "XPathNSResolver.h"
#include "XPathResult.h"
#include <array>
#include <wtf/HashSet.h>
#include <wtf/ListHashSet.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebCore {
namespace InteractionQuery {

using namespace HTMLNames;

static constexpr unsigned maximumFallbackNameLength = 120;

ReadScope::ReadScope(Page& page)
    : m_page(page)
    , m_previousMode(AXObjectCache::accessibilityMode())
{
    // Set directly: AXObjectCache::enableAccessibility() announces the change to every other process.
    if (isAccessibilityModeOff(m_previousMode))
        AXObjectCache::gAccessibilityMode.store(AccessibilityMode::MainThread, std::memory_order_relaxed);

    RefPtr mainFrame = m_page->localMainFrame();
    if (RefPtr view = mainFrame ? mainFrame->view() : nullptr)
        view->updateLayoutAndStyleIfNeededRecursive();
}

ReadScope::~ReadScope()
{
    if (!isAccessibilityModeOff(m_previousMode))
        return;

    // The tree goes together with the upkeep it queued for the objects the reads created. Left to
    // run, that upkeep stalls the page for a time that grows steeply with how deep the document nests.
    RefPtr mainFrame = m_page->localMainFrame();
    if (RefPtr document = mainFrame ? mainFrame->document() : nullptr)
        document->clearAXObjectCache();
    AXObjectCache::gAccessibilityMode.store(m_previousMode, std::memory_order_relaxed);
}

String refForElement(Element& element)
{
    return makeString('e', Process::identifier().toUInt64(), '-', element.nodeIdentifier().toUInt64());
}

RefPtr<Element> elementForRef(Page& page, const String& ref)
{
    auto separator = ref.find('-');
    if (!ref.startsWith('e') || separator == notFound)
        return nullptr;

    StringView view { ref };
    auto process = parseInteger<uint64_t>(view.substring(1, separator - 1));
    auto node = parseInteger<uint64_t>(view.substring(separator + 1));
    if (!process || !node || *process != Process::identifier().toUInt64() || !NodeIdentifier::isValidIdentifier(*node))
        return nullptr;

    RefPtr element = dynamicDowncast<Element>(Node::fromIdentifier(NodeIdentifier { *node }));
    if (!element || !element->isConnected())
        return nullptr;

    Ref document = element->document();
    if (document->page() != &page || !document->isFullyActive())
        return nullptr;
    return element;
}

static String collapseWhitespace(const String& text)
{
    return text.simplifyWhiteSpace(isASCIIWhitespace);
}

// Text never matches on letter case or on runs of whitespace; `exact` asks for the whole text rather than
// a part of it. `pattern` must already be whitespace-collapsed.
static bool matchesText(const String& text, const String& pattern, bool exact)
{
    auto candidate = collapseWhitespace(text);
    return exact ? equalIgnoringASCIICase(candidate, pattern) : candidate.containsIgnoringASCIICase(pattern);
}

static RefPtr<AccessibilityObject> accessibilityObject(Element& element)
{
    CheckedPtr cache = element.document().axObjectCache();
    return cache ? cache->getOrCreate(element) : nullptr;
}

// The text an author attached to an element as its label: aria-labelledby, then aria-label, then <label>s.
static String labelFor(Element& element)
{
    auto& labelledBy = element.attributeWithoutSynchronization(aria_labelledbyAttr);
    if (!labelledBy.isEmpty()) {
        StringBuilder text;
        for (auto& identifier : SpaceSplitString { labelledBy, SpaceSplitString::ShouldFoldCase::No }) {
            if (RefPtr labelling = element.treeScope().getElementById(identifier))
                text.append(labelling->textContent(), ' ');
        }
        if (auto label = collapseWhitespace(text.toString()); !label.isEmpty())
            return label;
    }

    if (auto label = collapseWhitespace(element.attributeWithoutSynchronization(aria_labelAttr)); !label.isEmpty())
        return label;

    RefPtr htmlElement = dynamicDowncast<HTMLElement>(element);
    RefPtr labels = htmlElement && htmlElement->isLabelable() ? htmlElement->labels() : nullptr;
    StringBuilder text;
    for (unsigned index = 0; labels && index < labels->length(); ++index) {
        if (RefPtr label = labels->item(index))
            text.append(label->textContent(), ' ');
    }
    return collapseWhitespace(text.toString());
}

static bool isDisabled(Element& element)
{
    if (element.isDisabledFormControl())
        return true;

    // aria-disabled applies to the element and its descendants until one of them says otherwise.
    for (RefPtr ancestor = &element; ancestor; ancestor = ancestor->parentElementInComposedTree()) {
        auto& value = ancestor->attributeWithoutSynchronization(aria_disabledAttr);
        if (equalLettersIgnoringASCIICase(value, "true"_s))
            return true;
        if (equalLettersIgnoringASCIICase(value, "false"_s))
            return false;
    }
    return false;
}

static bool isEditable(Element& element)
{
    if (element.isTextFormControlElement())
        return element.isTextField() && element.matchesReadWritePseudoClass();
    return element.isContentEditable();
}

bool isVisible(Element& element)
{
    CheckVisibilityOptions options;
    options.visibilityProperty = true;
    if (!element.renderer() || !element.checkVisibility(options))
        return false;

    CheckedPtr renderer = element.renderer();
    return renderer && !renderer->absoluteBoundingBoxRect().isEmpty();
}

static bool hasPointerListeners(Element& element)
{
    auto& names = eventNames();
    return element.hasEventListeners(names.clickEvent)
        || element.hasEventListeners(names.mousedownEvent)
        || element.hasEventListeners(names.mouseupEvent)
        || element.hasEventListeners(names.pointerdownEvent)
        || element.hasEventListeners(names.pointerupEvent);
}

// A listener on the element itself, or the outermost box of a `cursor: pointer` region: frameworks that
// delegate events to the document root leave the cursor as the only sign that something reacts to a click.
static bool looksClickable(Element& element)
{
    if (hasPointerListeners(element))
        return true;

    CheckedPtr renderer = element.renderer();
    if (!renderer)
        return false;

    CheckedRef style = renderer->style();
    if (style->cursorType() != CursorType::Pointer || style->pointerEvents() == PointerEvents::None)
        return false;

    CheckedPtr parent = renderer->parent();
    return !parent || parent->style().cursorType() != CursorType::Pointer;
}

static bool isInteractiveRole(const String& role)
{
    static constexpr std::array roles {
        "button"_s, "checkbox"_s, "combobox"_s, "link"_s, "listbox"_s, "menuitem"_s, "menuitemcheckbox"_s,
        "menuitemradio"_s, "option"_s, "radio"_s, "searchbox"_s, "slider"_s, "spinbutton"_s, "switch"_s,
        "tab"_s, "textbox"_s, "treeitem"_s,
    };
    for (auto candidate : roles) {
        if (role == candidate)
            return true;
    }
    return false;
}

static bool isInteractiveElement(Element& element)
{
    if (element.isLink() || is<HTMLButtonElement>(element) || is<HTMLSelectElement>(element) || is<HTMLTextAreaElement>(element) || is<HTMLOptionElement>(element))
        return true;
    if (RefPtr input = dynamicDowncast<HTMLInputElement>(element))
        return !input->isInputTypeHidden();
    if (RefPtr summary = dynamicDowncast<HTMLSummaryElement>(element))
        return summary->isActiveSummary();
    if (element.isRootEditableElement())
        return true;

    auto tabIndex = element.tabIndexSetExplicitly();
    return tabIndex && *tabIndex >= 0;
}

// What accessibility and the element itself say about a node, computed once per node.
struct Semantics {
    RefPtr<AccessibilityObject> object;
    String role;
    bool clickable { false };
    bool interactive { false };
    // Exposed to accessibility with a role of its own, so worth listing even when it is not interactive.
    bool exposed { false };
};

static Semantics semanticsFor(Element& element)
{
    Semantics semantics;
    semantics.object = accessibilityObject(element);
    // Accessibility calls a select a button whose name is its options; ARIA's roles say what it does.
    if (is<HTMLOptionElement>(element))
        semantics.role = "option"_s;
    else if (RefPtr select = dynamicDowncast<HTMLSelectElement>(element))
        semantics.role = select->usesMenuList() ? "combobox"_s : "listbox"_s;
    else if (semantics.object)
        semantics.role = semantics.object->computedRoleString();

    semantics.clickable = looksClickable(element);
    semantics.interactive = semantics.clickable || isInteractiveElement(element) || isInteractiveRole(semantics.role);
    semantics.exposed = !semantics.role.isEmpty() && semantics.object && !semantics.object->isIgnored();
    return semantics;
}

// The first text the accessibility tree has for an object is its name. AccessibilityObject::computedLabel()
// answers the same after running the tree's deferred upkeep, which a tree made for one read never needs.
static String accessibleName(const AccessibilityObject& object)
{
    Vector<AccessibilityText> texts;
    object.accessibilityText(texts);
    return texts.isEmpty() ? String() : texts[0].text;
}

static String nameFor(Element& element, const Semantics& semantics)
{
    if (RefPtr option = dynamicDowncast<HTMLOptionElement>(element))
        return collapseWhitespace(option->displayLabel());
    if (is<HTMLSelectElement>(element))
        return labelFor(element);

    auto name = semantics.object ? collapseWhitespace(accessibleName(*semantics.object)) : String();
    if (!name.isEmpty() || !semantics.interactive)
        return name;

    // Controls that accessibility leaves unnamed still need something to be told apart by.
    if (element.isTextFormControlElement())
        return collapseWhitespace(element.attributeWithoutSynchronization(placeholderAttr));

    name = collapseWhitespace(element.innerText());
    if (name.length() > maximumFallbackNameLength)
        name = makeString(StringView { name }.left(maximumFallbackNameLength), "\u2026"_s);
    return name;
}

// What a form control holds, empty included; std::nullopt for anything that holds no value.
static std::optional<String> formControlValue(Element& element)
{
    String value;
    if (RefPtr input = dynamicDowncast<HTMLInputElement>(element))
        value = input->value();
    else if (RefPtr textArea = dynamicDowncast<HTMLTextAreaElement>(element))
        value = textArea->value();
    else if (RefPtr select = dynamicDowncast<HTMLSelectElement>(element))
        value = select->value();
    else
        return std::nullopt;
    return value.isNull() ? emptyString() : value;
}

// The value worth showing in a snapshot: what a control currently holds, never a password.
static String displayValue(Element& element)
{
    if (RefPtr input = dynamicDowncast<HTMLInputElement>(element)) {
        if (input->isCheckbox() || input->isRadioButton() || input->isTextButton() || input->isInputTypeHidden() || input->isPasswordField())
            return { };
        return input->value();
    }
    if (is<HTMLTextAreaElement>(element) || is<HTMLSelectElement>(element))
        return formControlValue(element).value_or(String { });

    auto& valueText = element.attributeWithoutSynchronization(aria_valuetextAttr);
    return valueText.isEmpty() ? element.attributeWithoutSynchronization(aria_valuenowAttr).string() : valueText.string();
}

// Tri-state: std::nullopt when the node is not checkable.
static std::optional<AccessibilityButtonState> checkedState(Element& element, AccessibilityObject* object)
{
    if (RefPtr input = dynamicDowncast<HTMLInputElement>(element); input && (input->isCheckbox() || input->isRadioButton())) {
        if (input->indeterminate())
            return AccessibilityButtonState::Mixed;
        return input->checked() ? AccessibilityButtonState::On : AccessibilityButtonState::Off;
    }
    if (!object || !object->supportsChecked())
        return std::nullopt;

    auto state = object->checkboxOrRadioValue();
    if (state == AccessibilityButtonState::Off && object->isChecked())
        return AccessibilityButtonState::On;
    return state;
}

static Ref<JSON::Array> statesFor(Element& element, const Semantics& semantics)
{
    auto states = JSON::Array::create();
    auto add = [&](ASCIILiteral state) {
        states->pushString(state);
    };

    if (isDisabled(element))
        add("disabled"_s);
    if (element.document().focusedElement() == &element)
        add("focused"_s);
    if (isEditable(element))
        add("editable"_s);
    if (semantics.clickable)
        add("clickable"_s);
    if (is<HTMLTextAreaElement>(element))
        add("multiline"_s);

    if (RefPtr option = dynamicDowncast<HTMLOptionElement>(element)) {
        if (option->selected())
            add("selected"_s);
        return states;
    }

    RefPtr object = semantics.object;
    if (auto checked = checkedState(element, object.get())) {
        if (*checked == AccessibilityButtonState::On)
            add("checked"_s);
        else if (*checked == AccessibilityButtonState::Mixed)
            add("mixed"_s);
    }
    if (!object)
        return states;

    if (object->pressedIsPresent() && object->isPressed())
        add("pressed"_s);
    if (object->supportsExpanded())
        add(object->isExpanded() ? "expanded"_s : "collapsed"_s);
    if (object->isSelected())
        add("selected"_s);
    if (object->supportsRequiredAttribute() && object->isRequired())
        add("required"_s);
    if (object->isTextControl() && !object->canSetValueAttribute())
        add("readonly"_s);
    // Only what the author declared: a required field that is still empty is not an error yet.
    auto& invalid = element.attributeWithoutSynchronization(aria_invalidAttr);
    if (!invalid.isEmpty() && !equalLettersIgnoringASCIICase(invalid, "false"_s))
        add("invalid"_s);
    if (!is<HTMLTextAreaElement>(element) && equalLettersIgnoringASCIICase(element.attributeWithoutSynchronization(aria_multilineAttr), "true"_s))
        add("multiline"_s);
    if (object->isMultiSelectable())
        add("multiselectable"_s);
    if (object->hasPopup())
        add("haspopup"_s);
    if (object->isBusy())
        add("busy"_s);
    if (object->isModalNode())
        add("modal"_s);
    return states;
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

// An option in a pop-up menu has no box of its own: it stands where its select does.
static IntRect boundsInRootView(Element& element)
{
    if (RefPtr option = dynamicDowncast<HTMLOptionElement>(element); option && !option->renderer()) {
        if (RefPtr select = option->ownerSelectElement())
            return select->boundingBoxInRootViewCoordinates();
    }
    return element.boundingBoxInRootViewCoordinates();
}

static Ref<JSON::Object> describe(Element& element, const Semantics& semantics, OptionSet<Include> include)
{
    auto node = JSON::Object::create();
    node->setString("ref"_s, refForElement(element));
    node->setString("tag"_s, element.localName());
    node->setString("role"_s, semantics.role.isEmpty() ? String { "generic"_s } : semantics.role);

    if (auto name = nameFor(element, semantics); !name.isEmpty())
        node->setString("name"_s, name);
    if (auto value = displayValue(element); !value.isEmpty())
        node->setString("value"_s, value);

    if (RefPtr object = semantics.object) {
        if (auto description = collapseWhitespace(object->helpText()); !description.isEmpty())
            node->setString("description"_s, description);
        if (auto shortcuts = object->keyShortcuts(); !shortcuts.isEmpty())
            node->setString("keyshortcuts"_s, shortcuts);
    }

    node->setArray("states"_s, statesFor(element, semantics));
    node->setArray("rect"_s, rectAsJSON(boundsInRootView(element)));

    if (include.contains(Include::Text))
        node->setString("text"_s, element.innerText());
    if (include.contains(Include::HTML))
        node->setString("html"_s, element.outerHTML());
    if (include.contains(Include::Value)) {
        if (auto value = formControlValue(element))
            node->setString("value"_s, *value);
    }
    if (include.contains(Include::Attributes)) {
        auto attributes = JSON::Object::create();
        for (auto& name : element.getAttributeNames())
            attributes->setString(name, element.getAttribute(AtomString { name }));
        node->setObject("attributes"_s, WTF::move(attributes));
    }
    if (include.contains(Include::Checked)) {
        if (auto checked = checkedState(element, semantics.object.get())) {
            if (*checked == AccessibilityButtonState::Mixed)
                node->setString("checked"_s, "mixed"_s);
            else
                node->setBoolean("checked"_s, *checked == AccessibilityButtonState::On);
        }
    }
    return node;
}

Ref<JSON::Object> describe(Element& element, OptionSet<Include> include)
{
    return describe(element, semanticsFor(element), include);
}

// The children a user would see under an element: the flat tree, with a frame's document standing
// in for the frame and without the engine's own shadow content.
static Vector<Ref<Node>> childrenInFlatTree(Element& element)
{
    Vector<Ref<Node>> children;

    if (RefPtr owner = dynamicDowncast<HTMLFrameOwnerElement>(element)) {
        RefPtr document = owner->contentDocument();
        if (RefPtr root = document ? document->documentElement() : nullptr)
            children.append(root.releaseNonNull());
        return children;
    }

    RefPtr shadowRoot = element.shadowRoot();
    if (shadowRoot && shadowRoot->mode() == ShadowRootMode::UserAgent) {
        for (RefPtr child = element.firstChild(); child; child = child->nextSibling())
            children.append(*child);
        return children;
    }

    for (auto& child : composedTreeChildren(element))
        children.append(child);
    return children;
}

// How deep the walks below go. A document is neither styled nor rendered below this depth, so nothing
// further down can be seen or take input, while script can nest elements far deeper than the stack
// allows recursing.
static constexpr unsigned maximumDepth = Settings::defaultMaximumRenderTreeDepth;

static bool hasContentDocument(Element& element)
{
    RefPtr owner = dynamicDowncast<HTMLFrameOwnerElement>(element);
    return owner && owner->contentDocument();
}

struct SnapshotWalk {
    const SnapshotOptions& options;
    IntRect viewport;
    unsigned emitted { 0 };
    bool truncated { false };
    bool tooDeep { false };

    bool take()
    {
        if (emitted >= options.limit) {
            truncated = true;
            return false;
        }
        ++emitted;
        return true;
    }

    bool shows(const IntRect& rect) const { return !options.viewportOnly || rect.intersects(viewport); }
};

static void appendNode(Node&, JSON::Array& siblings, SnapshotWalk&, const String& enclosingName, unsigned depth);

static void appendText(Text& text, JSON::Array& siblings, SnapshotWalk& walk, const String& enclosingName)
{
    CheckedPtr renderer = text.renderer();
    RefPtr view = text.document().view();
    if (!walk.options.includeAll || !renderer || !view)
        return;

    // Text that only repeats the name of the node around it adds nothing.
    auto content = collapseWhitespace(text.data());
    if (content.isEmpty() || content == enclosingName)
        return;

    auto rect = view->contentsToRootView(renderer->absoluteBoundingBoxRect());
    if (!walk.shows(rect) || !walk.take())
        return;

    auto leaf = JSON::Object::create();
    leaf->setString("role"_s, "text"_s);
    leaf->setString("name"_s, content);
    leaf->setArray("rect"_s, rectAsJSON(rect));
    siblings.pushObject(WTF::move(leaf));
}

static void appendNode(Node& node, JSON::Array& siblings, SnapshotWalk& walk, const String& enclosingName, unsigned depth)
{
    if (walk.truncated)
        return;
    if (depth > maximumDepth) {
        walk.tooDeep = true;
        return;
    }

    if (RefPtr text = dynamicDowncast<Text>(node)) {
        appendText(*text, siblings, walk, enclosingName);
        return;
    }

    RefPtr element = dynamicDowncast<Element>(node);
    if (!element)
        return;

    // Nothing under `display: none` is on screen; `display: contents` only removes the element's own box.
    bool isOption = is<HTMLOptionElement>(*element);
    bool rendered = element->renderer() || isOption;
    if (!rendered && !element->hasDisplayContents())
        return;

    auto semantics = semanticsFor(*element);
    bool isFrame = hasContentDocument(*element);
    bool emit = rendered && (isFrame || semantics.interactive || (walk.options.includeAll && semantics.exposed));
    if (emit && !isOption)
        emit = isVisible(*element);
    if (emit)
        emit = walk.shows(boundsInRootView(*element));
    if (emit && !walk.take())
        return;

    // A node that is not listed passes its children up to the nearest listed ancestor.
    RefPtr<JSON::Object> described;
    RefPtr<JSON::Array> children;
    String name = enclosingName;
    if (emit) {
        described = describe(*element, semantics, { });
        children = JSON::Array::create();
        name = described->getString("name"_s);
    }

    for (auto& child : childrenInFlatTree(*element))
        appendNode(child, emit ? *children : siblings, walk, name, depth + 1);

    if (!emit)
        return;
    if (isFrame)
        described->setBoolean("frame"_s, true);
    if (children->length())
        described->setArray("children"_s, children.releaseNonNull());
    siblings.pushObject(described.releaseNonNull());
}

Ref<JSON::Object> snapshot(Page& page, const SnapshotOptions& options)
{
    auto reply = JSON::Object::create();
    auto nodes = JSON::Array::create();

    RefPtr mainFrame = page.localMainFrame();
    RefPtr document = mainFrame ? mainFrame->document() : nullptr;
    RefPtr view = mainFrame ? mainFrame->view() : nullptr;
    if (!document || !view) {
        reply->setArray("nodes"_s, WTF::move(nodes));
        reply->setBoolean("truncated"_s, false);
        return reply;
    }

    auto visible = view->visibleContentRect();
    auto contents = view->contentsSize();

    auto viewport = JSON::Object::create();
    viewport->setInteger("width"_s, visible.width());
    viewport->setInteger("height"_s, visible.height());
    viewport->setDouble("scale"_s, page.deviceScaleFactor());

    auto scroll = JSON::Object::create();
    scroll->setInteger("x"_s, visible.x());
    scroll->setInteger("y"_s, visible.y());
    scroll->setInteger("width"_s, visible.width());
    scroll->setInteger("height"_s, visible.height());
    scroll->setInteger("scrollWidth"_s, contents.width());
    scroll->setInteger("scrollHeight"_s, contents.height());

    SnapshotWalk walk { options, view->contentsToRootView(visible) };
    RefPtr root = options.root;
    if (!root)
        root = document->documentElement();
    if (root)
        appendNode(*root, nodes, walk, { }, 0);

    reply->setString("url"_s, document->url().string());
    reply->setString("title"_s, document->title());
    reply->setObject("viewport"_s, WTF::move(viewport));
    reply->setObject("scroll"_s, WTF::move(scroll));
    reply->setArray("nodes"_s, WTF::move(nodes));
    reply->setBoolean("truncated"_s, walk.truncated || walk.tooDeep);
    return reply;
}

// The part of an element that its own frame, every enclosing frame and all overflow clips leave on screen.
static IntRect visibleRectInRootView(Element& element)
{
    CheckedPtr renderer = element.renderer();
    RefPtr view = element.document().view();
    if (!renderer || !view)
        return { };

    auto rect = renderer->absoluteBoundingBoxRect();
    rect.intersect(snappedIntRect(renderer->absoluteClippedOverflowRectForSpatialNavigation()));
    rect.intersect(view->visibleContentRect());

    auto visible = view->contentsToRootView(rect);
    if (RefPtr owner = element.document().ownerElement())
        visible.intersect(visibleRectInRootView(*owner));
    return visible;
}

// An inline box that wraps has a bounding box whose centre can lie outside every line, so aim at
// the middle of its first fragment that is on screen.
static FloatPoint aimPoint(Element& element, const IntRect& visible)
{
    CheckedPtr renderer = element.renderer();
    RefPtr view = element.document().view();
    if (renderer && view) {
        Vector<FloatQuad> quads;
        renderer->absoluteQuads(quads);
        for (auto& quad : quads) {
            auto fragment = intersection(view->contentsToRootView(quad.enclosingBoundingBox()), visible);
            if (!fragment.isEmpty())
                return FloatRect { fragment }.center();
        }
    }
    return FloatRect { visible }.center();
}

static RefPtr<Element> elementAtPoint(LocalFrame& mainFrame, FloatPoint pointInRootView)
{
    RefPtr view = mainFrame.view();
    if (!view)
        return nullptr;

    static constexpr OptionSet hitType {
        HitTestRequest::Type::ReadOnly,
        HitTestRequest::Type::Active,
        HitTestRequest::Type::DisallowUserAgentShadowContent,
        HitTestRequest::Type::AllowChildFrameContent,
    };
    auto result = mainFrame.eventHandler().hitTestResultAtPoint(view->rootViewToContents(roundedIntPoint(pointInRootView)), hitType);
    return result.targetElement();
}

// Whether a pointer event delivered to `hit` acts on `target`.
static bool reaches(Element& hit, Element& target)
{
    if (target.isShadowIncludingInclusiveAncestorOf(hit))
        return true;

    // Content inside a frame belongs to the frame's owner in its parent document.
    for (RefPtr owner = hit.document().ownerElement(); owner; owner = owner->document().ownerElement()) {
        if (target.isShadowIncludingInclusiveAncestorOf(*owner))
            return true;
    }

    // A click on a label activates its control.
    for (RefPtr ancestor = &hit; ancestor; ancestor = ancestor->parentElementInComposedTree()) {
        if (RefPtr label = dynamicDowncast<HTMLLabelElement>(*ancestor))
            return label->control() == &target;
    }
    return false;
}

// A box or a frame view whose scroll position moves content on screen.
struct Scroller {
    ScrollableArea* area { nullptr };
    IntRect viewport; // Root view coordinates, not clipped by enclosing scrollers.
    bool scrollsX { false };
    bool scrollsY { false };
};

enum class IncludeSelf : bool { No, Yes };

static void appendIfScroller(Vector<Scroller>& scrollers, const RenderElement& renderer, LocalFrameView& view)
{
    CheckedPtr box = dynamicDowncast<RenderBox>(renderer);
    if (!box || !box->hasScrollableOverflow() || !box->layer())
        return;

    auto* area = box->layer()->scrollableArea();
    if (!area)
        return;

    auto viewport = box->localToAbsoluteQuad(FloatQuad { FloatRect { box->paddingBoxRect() } }).enclosingBoundingBox();
    scrollers.append({ area, view.contentsToRootView(viewport), box->hasScrollableOverflowX(), box->hasScrollableOverflowY() });
}

// Innermost first: everything that scrolls `renderer`, through every enclosing frame. The walk follows
// containers rather than parents because a scroller does not move positioned descendants it does not contain.
static Vector<Scroller> scrollersMoving(const RenderElement& renderer, IncludeSelf includeSelf = IncludeSelf::No)
{
    Vector<Scroller> scrollers;
    CheckedPtr<const RenderElement> current = &renderer;
    bool includeCurrent = includeSelf == IncludeSelf::Yes;

    while (current) {
        Ref document = current->document();
        RefPtr view = document->view();
        if (!view)
            break;

        // A fixed-position box does not move when its frame scrolls.
        bool isFixed = false;
        current->absoluteBoundingBoxRect(true, &isFixed);

        if (includeCurrent)
            appendIfScroller(scrollers, *current, *view);
        for (CheckedPtr container = current->container(); container && !container->isRenderView(); container = container->container())
            appendIfScroller(scrollers, *container, *view);
        if (!isFixed)
            scrollers.append({ view.get(), view->contentsToRootView(view->visibleContentRect()), true, true });

        RefPtr owner = document->ownerElement();
        current = owner ? owner->renderer() : nullptr;
        includeCurrent = false;
    }
    return scrollers;
}

static FloatSize clampToScrollRange(const Scroller& scroller, FloatSize delta)
{
    auto position = scroller.area->scrollPosition();
    auto minimum = scroller.area->minimumScrollPosition();
    auto maximum = scroller.area->maximumScrollPosition();

    FloatSize clamped;
    if (scroller.scrollsX)
        clamped.setWidth(std::clamp<float>(position.x() + delta.width(), minimum.x(), maximum.x()) - position.x());
    if (scroller.scrollsY)
        clamped.setHeight(std::clamp<float>(position.y() + delta.height(), minimum.y(), maximum.y()) - position.y());
    return clamped;
}

static bool isNegligible(FloatSize delta)
{
    return std::abs(delta.width()) < 1 && std::abs(delta.height()) < 1;
}

// The wheel delta that brings the middle of `focus` to the middle of `viewport`, as far as the scroller can
// still go. An axis along which `focus` already fits inside `viewport` is left alone.
static FloatSize deltaToCentre(const Scroller& scroller, const IntRect& viewport, const IntRect& focus)
{
    FloatSize delta;
    if (focus.x() < viewport.x() || focus.maxX() > viewport.maxX())
        delta.setWidth(focus.center().x() - viewport.center().x());
    if (focus.y() < viewport.y() || focus.maxY() > viewport.maxY())
        delta.setHeight(focus.center().y() - viewport.center().y());
    return clampToScrollRange(scroller, delta);
}

// A point inside `viewport` where a wheel event scrolls `scroller` rather than something nested in it.
static FloatPoint wheelPoint(LocalFrame& mainFrame, const Scroller& scroller, const IntRect& viewport, FloatSize delta)
{
    FloatRect area { viewport };
    auto centre = area.center();
    const std::array candidates {
        centre,
        FloatPoint { area.x() + area.width() / 4, area.y() + area.height() / 4 },
        FloatPoint { area.x() + 3 * area.width() / 4, area.y() + area.height() / 4 },
        FloatPoint { area.x() + area.width() / 4, area.y() + 3 * area.height() / 4 },
        FloatPoint { area.x() + 3 * area.width() / 4, area.y() + 3 * area.height() / 4 },
    };

    for (auto candidate : candidates) {
        RefPtr hit = elementAtPoint(mainFrame, candidate);
        CheckedPtr renderer = hit ? hit->renderer() : nullptr;
        if (!renderer)
            continue;

        for (auto& underPoint : scrollersMoving(*renderer, IncludeSelf::Yes)) {
            if (underPoint.area == scroller.area)
                return candidate;
            // A nested scroller that can still move in this direction takes the wheel first.
            if (!isNegligible(clampToScrollRange(underPoint, delta)))
                break;
        }
    }
    return centre;
}

// The first wheel towards showing an element that no scroller currently shows, working from the outermost
// scroller in: each level must show the next scroller in before that one can be scrolled.
static std::optional<Scroll> scrollToReveal(Element& element, LocalFrame& mainFrame, const IntRect& pageViewport)
{
    CheckedPtr renderer = element.renderer();
    if (!renderer)
        return std::nullopt;

    auto scrollers = scrollersMoving(*renderer);
    auto target = element.boundingBoxInRootViewCoordinates();
    auto clip = pageViewport;

    for (size_t index = scrollers.size(); index--;) {
        auto& scroller = scrollers[index];
        auto viewport = intersection(scroller.viewport, clip);
        if (viewport.isEmpty())
            return std::nullopt;

        auto focus = target;
        if (index && !scrollers[index - 1].viewport.intersects(target))
            focus = scrollers[index - 1].viewport;

        if (!viewport.intersects(focus)) {
            if (auto delta = deltaToCentre(scroller, viewport, focus); !isNegligible(delta))
                return Scroll { wheelPoint(mainFrame, scroller, viewport, delta), delta };
        }
        clip = viewport;
    }
    return std::nullopt;
}

// A wheel that centres an element which is on screen but overlapped, for one more attempt at reaching it.
static std::optional<Scroll> scrollToCentre(Element& element, LocalFrame& mainFrame, const IntRect& pageViewport)
{
    CheckedPtr renderer = element.renderer();
    if (!renderer)
        return std::nullopt;

    auto target = element.boundingBoxInRootViewCoordinates();
    for (auto& scroller : scrollersMoving(*renderer)) {
        auto viewport = intersection(scroller.viewport, pageViewport);
        if (viewport.isEmpty())
            continue;

        auto delta = clampToScrollRange(scroller, FloatSize { target.center() - viewport.center() });
        if (!isNegligible(delta))
            return Scroll { wheelPoint(mainFrame, scroller, viewport, delta), delta };
    }
    return std::nullopt;
}

Target resolve(Element& element, Action action)
{
    Target target;

    RefPtr mainFrame = element.document().localMainFrame();
    RefPtr mainView = mainFrame ? mainFrame->view() : nullptr;
    if (!mainView || !isVisible(element))
        return target;

    if ((action == Action::Click || action == Action::Type) && isDisabled(element)) {
        target.state = TargetState::Disabled;
        return target;
    }
    if (action == Action::Type && !isEditable(element)) {
        target.state = TargetState::NotEditable;
        return target;
    }

    auto pageViewport = mainView->contentsToRootView(mainView->visibleContentRect());
    auto visible = intersection(visibleRectInRootView(element), pageViewport);
    if (visible.isEmpty()) {
        // Either a scroll brings it into view, or a clip that does not scroll hides it for good.
        if (auto scroll = scrollToReveal(element, *mainFrame, pageViewport)) {
            target.state = TargetState::OffViewport;
            target.scroll = WTF::move(scroll);
        }
        return target;
    }

    target.rect = visible;
    target.point = aimPoint(element, visible);

    if (action != Action::None) {
        RefPtr hit = elementAtPoint(*mainFrame, target.point);
        if (!hit || !reaches(*hit, element)) {
            target.state = TargetState::Covered;
            target.coveredBy = WTF::move(hit);
            target.scroll = scrollToCentre(element, *mainFrame, pageViewport);
            return target;
        }
    }

    target.state = TargetState::Ok;
    return target;
}

// The element a query starts from and whether that element itself may match: a frame owner stands for
// its document, an explicit root only scopes the search.
struct Scope {
    RefPtr<Element> root;
    bool includeRoot { true };
};

static Scope scopeFor(Page& page, Element* root)
{
    if (!root) {
        RefPtr mainFrame = page.localMainFrame();
        RefPtr document = mainFrame ? mainFrame->document() : nullptr;
        return { document ? document->documentElement() : nullptr, true };
    }
    if (RefPtr owner = dynamicDowncast<HTMLFrameOwnerElement>(*root)) {
        RefPtr document = owner->contentDocument();
        return { document ? document->documentElement() : nullptr, true };
    }
    return { root, false };
}

// Visits `root` and the elements under it in document order.
template<typename Visitor>
static void forEachElementInFlatTree(Element& root, const Visitor& visit)
{
    Vector<std::pair<Ref<Element>, unsigned>> pending;
    pending.append({ root, 0 });
    while (!pending.isEmpty()) {
        auto [element, depth] = pending.takeLast();
        visit(element.get());
        if (depth == maximumDepth)
            continue;

        auto children = childrenInFlatTree(element.get());
        for (size_t index = children.size(); index--;) {
            if (RefPtr child = dynamicDowncast<Element>(children[index].get()))
                pending.append({ child.releaseNonNull(), depth + 1 });
        }
    }
}

// The elements under `root`, itself included, that a selector matches. Each tree scope is queried once,
// the way querySelectorAll() does it, so that what matching one element learned about a subtree serves
// the others; asking element by element makes selectors like `:has()` quadratic.
static std::expected<Vector<Ref<Element>>, String> evaluateSelector(Element& root, const String& selector)
{
    Vector<Ref<Element>> elements;
    ListHashSet<Ref<ContainerNode>> scopes;
    forEachElementInFlatTree(root, [&](Element& element) {
        elements.append(element);
        scopes.add(element.treeScope().rootNode());
    });

    HashSet<Ref<Element>> matching;
    for (auto& scope : scopes) {
        auto query = scope->document().selectorQueryForString(selector);
        if (query.hasException())
            return makeUnexpected(makeString("invalid selector: "_s, selector));

        Ref list = query.returnValue().queryAll(scope);
        for (unsigned index = 0; index < list->length(); ++index) {
            if (RefPtr element = dynamicDowncast<Element>(list->item(index)))
                matching.add(element.releaseNonNull());
        }
    }

    elements.removeAllMatching([&](auto& element) {
        return !matching.contains(element);
    });
    return elements;
}

// Appends the innermost elements under `element` whose rendered text matches, and returns that text.
static String collectTextMatches(Element& element, const String& pattern, bool exact, Vector<Ref<Element>>& matches, bool& matched, unsigned depth)
{
    StringBuilder text;
    for (auto& child : childrenInFlatTree(element)) {
        if (RefPtr childText = dynamicDowncast<Text>(child.get())) {
            if (childText->renderer())
                text.append(childText->data());
            continue;
        }

        RefPtr childElement = dynamicDowncast<Element>(child.get());
        if (!childElement || depth == maximumDepth || (!childElement->renderer() && !childElement->hasDisplayContents()))
            continue;

        // Words in neighbouring blocks do not run together; pieces of one line do.
        bool isInline = !childElement->renderer() || childElement->renderer()->isInline();
        bool matchedInChild = false;
        auto childContent = collectTextMatches(*childElement, pattern, exact, matches, matchedInChild, depth + 1);
        if (isInline)
            text.append(childContent);
        else
            text.append(' ', childContent, ' ');
        matched |= matchedInChild;
    }

    auto content = text.toString();
    if (!matched && element.renderer() && matchesText(content, pattern, exact)) {
        matches.append(element);
        matched = true;
    }
    return content;
}

static std::expected<Vector<Ref<Element>>, String> evaluateXPath(Element& root, const String& expression)
{
    Vector<Ref<Element>> matches;
    Vector<Ref<Node>> contexts;
    contexts.append(root);

    // XPath does not cross documents: evaluate once per frame, in document order of the frames' owners.
    for (size_t index = 0; index < contexts.size(); ++index) {
        Ref context = contexts[index];
        Ref document = context->document();

        auto result = document->evaluate(expression, context, nullptr, XPathResult::ORDERED_NODE_SNAPSHOT_TYPE, nullptr);
        if (result.hasException())
            return makeUnexpected(makeString("invalid XPath expression: "_s, expression));

        Ref snapshot = result.releaseReturnValue();
        auto length = snapshot->snapshotLength();
        for (unsigned item = 0; !length.hasException() && item < length.returnValue(); ++item) {
            auto node = snapshot->snapshotItem(item);
            if (node.hasException())
                break;
            if (RefPtr element = dynamicDowncast<Element>(node.returnValue()))
                matches.append(element.releaseNonNull());
        }

        if (RefPtr container = dynamicDowncast<ContainerNode>(context.get())) {
            for (Ref owner : descendantsOfType<HTMLFrameOwnerElement>(*container)) {
                RefPtr frameDocument = owner->contentDocument();
                if (RefPtr frameRoot = frameDocument ? frameDocument->documentElement() : nullptr)
                    contexts.append(frameRoot.releaseNonNull());
            }
        }
    }
    return matches;
}

std::expected<Vector<Ref<Element>>, String> query(Page& page, const Query& query)
{
    Vector<Ref<Element>> matches;

    if (query.kind == QueryKind::Ref) {
        if (RefPtr element = elementForRef(page, query.value))
            matches.append(element.releaseNonNull());
        return matches;
    }

    auto scope = scopeFor(page, query.root.get());
    RefPtr root = scope.root;
    if (!root)
        return matches;

    auto pattern = collapseWhitespace(query.value);
    auto collect = [&](const auto& matchesElement) {
        forEachElementInFlatTree(*root, [&](Element& element) {
            if (matchesElement(element))
                matches.append(element);
        });
    };

    switch (query.kind) {
    case QueryKind::CSS: {
        auto result = evaluateSelector(*root, query.value);
        if (!result)
            return result;
        matches = WTF::move(*result);
        break;
    }
    case QueryKind::Role: {
        if (pattern.isEmpty() && query.name.isNull())
            return makeUnexpected(String { "a role query needs a role or a name"_s });
        auto name = collapseWhitespace(query.name);
        collect([&](Element& element) {
            auto semantics = semanticsFor(element);
            if (pattern.isEmpty() ? !semantics.exposed : !equalIgnoringASCIICase(semantics.role, pattern))
                return false;
            return query.name.isNull() || matchesText(nameFor(element, semantics), name, query.exact);
        });
        break;
    }
    case QueryKind::Label:
        collect([&](Element& element) {
            auto label = labelFor(element);
            return !label.isEmpty() && matchesText(label, pattern, query.exact);
        });
        break;
    case QueryKind::Attribute: {
        if (query.attribute.isEmpty())
            return makeUnexpected(String { "an attribute query needs an attribute name"_s });
        AtomString attribute { query.attribute };
        collect([&](Element& element) {
            auto& value = element.getAttribute(attribute);
            return !value.isNull() && matchesText(value.string(), pattern, query.exact);
        });
        break;
    }
    case QueryKind::XPath: {
        auto result = evaluateXPath(*root, query.value);
        if (!result)
            return result;
        matches = WTF::move(*result);
        break;
    }
    case QueryKind::Text: {
        if (pattern.isEmpty())
            return makeUnexpected(String { "a text query needs text"_s });
        bool matched = false;
        collectTextMatches(*root, pattern, query.exact, matches, matched, 0);
        break;
    }
    case QueryKind::Ref:
        break;
    }

    matches.removeAllMatching([&](auto& element) {
        return (!scope.includeRoot && element.ptr() == root.get()) || (query.visibleOnly && !isVisible(element));
    });
    return matches;
}

} // namespace InteractionQuery
} // namespace WebCore
