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

#include "Block.h"

#include "Display.h"
#include "Utilities.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace AgentHost {

const char isolatedWorldName[] = "ABHost";

// A page whose web process has not answered a round trip for this long is unresponsive (the engine's own
// responsiveness timeout); while calls into the page are pending, round trips follow each other this far apart.
static constexpr unsigned unresponsivePatience = 3000;
static constexpr unsigned pingInterval = 1000;

Block::Block(Host& host, uint64_t identifier, unsigned width, unsigned height, double scale, GRef<WebKitNetworkSession>&& session)
    : m_host(host)
    , m_identifier(identifier)
    , m_width(width)
    , m_height(height)
    , m_scale(scale)
    , m_session(std::move(session))
{
}

Block::~Block()
{
    close();
}

void Block::setFeature(const char* identifier, bool enabled)
{
    // A Web Inspector's page has the engine's settings.
    if (!m_settings)
        return;
    static WebKitFeatureList* features = webkit_settings_get_all_features();
    GChars longIdentifier(g_strconcat(identifier, "Enabled", nullptr));
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        auto* feature = webkit_feature_list_get(features, i);
        const char* name = webkit_feature_get_identifier(feature);
        if (!strcmp(name, identifier) || !strcmp(name, longIdentifier.get())) {
            webkit_settings_set_feature_enabled(m_settings.get(), feature, enabled);
            return;
        }
    }
    g_warning("the engine has no feature named %s", identifier);
}

#if defined(_WIN32)
void Block::drawWithoutGPU()
{
    setFeature("HardwareAcceleration", false);
    m_drawsWithGPU = false;
}
#endif

void Block::open(const std::string& url)
{
    m_settings = GRef<WebKitSettings>::adopt(webkit_settings_new());
    // A remote inspector reaches a page only through the instrumentation developer extras turn on.
    if (m_host.inspectorEnabled())
        webkit_settings_set_enable_developer_extras(m_settings.get(), TRUE);
    // The page is always focused in an active window, and renders no frame nobody asked for.
    setFeature("FocusEmulation", true);
    setFeature("CompositionSuppressed", true);
#if defined(_WIN32)
    if (!m_host.drawsWithGPU())
        drawWithoutGPU();
#endif
    applyDefaultIdentity();

    m_contentManager = GRef<WebKitUserContentManager>::adopt(webkit_user_content_manager_new());
    installHostScripts();

    setNewViewSize(m_host.display(), static_cast<int>(m_width), static_cast<int>(m_height));
    m_view = GRef<WebKitWebView>::adopt(WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", m_host.display(),
        "web-context", m_host.context(),
        "network-session", m_session.get(),
        "settings", m_settings.get(),
        "user-content-manager", m_contentManager.get(),
        nullptr)));
    m_wpeView = webkit_web_view_get_wpe_view(m_view.get());
    if (m_scale != 1)
        setViewScale(m_wpeView, m_scale);
    attachView();
    connectSignals();
    if (url != "about:blank") {
        webkit_web_view_load_uri(m_view.get(), url.c_str());
        return;
    }
    // A new view shows the empty document already. Loading it would make a history entry that a new tab does not
    // have; the two events a client waits for after `open` are reported without one.
    for (const char* type : { "committed", "loaded" }) {
        auto event = eventOfType(type);
        event.setString("url", url);
        record(std::move(event));
    }
}

// The items of a menu WPEView::context-menu describes, as an event carries them.
static Json menuItems(GVariant* items)
{
    auto list = Json::array();
    GVariantIter iter;
    g_variant_iter_init(&iter, items);
    while (GVariant* entry = g_variant_iter_next_value(&iter)) {
        auto item = Json::object();
        gboolean flag;
        guint32 id;
        const char* label;
        GVariant* submenu;
        if (g_variant_lookup(entry, "separator", "b", &flag) && flag)
            item.setBoolean("separator", true);
        else {
            if (g_variant_lookup(entry, "id", "u", &id))
                item.setNumber("id", id);
            if (g_variant_lookup(entry, "label", "&s", &label))
                item.setString("label", label);
            for (const char* name : { "enabled", "checked", "checkable" }) {
                if (g_variant_lookup(entry, name, "b", &flag))
                    item.setBoolean(name, flag);
            }
            if (g_variant_lookup(entry, "submenu", "@aa{sv}", &submenu)) {
                item.set("submenu", menuItems(submenu));
                g_variant_unref(submenu);
            }
        }
        list.append(item);
        g_variant_unref(entry);
    }
    return list;
}

void Block::attachView()
{
    // A page that brings items of its own (a Web Inspector's) asks the platform for its menu: the client shows
    // it, and says the item chosen (`menu`).
    auto contextMenu = +[](WPEView*, GVariant* menu, gpointer block) -> gboolean {
        auto event = eventOfType("contextMenu");
        double x = 0;
        double y = 0;
        if (g_variant_lookup(menu, "x", "d", &x) && g_variant_lookup(menu, "y", "d", &y)) {
            event.setNumber("x", x);
            event.setNumber("y", y);
        }
        if (GVariant* items = g_variant_lookup_value(menu, "items", G_VARIANT_TYPE("aa{sv}"))) {
            event.set("items", menuItems(items));
            g_variant_unref(items);
        }
        static_cast<Block*>(block)->record(std::move(event));
        return TRUE;
    };
    g_signal_connect(m_wpeView, "context-menu", G_CALLBACK(contextMenu), this);
    setFrameHandler(m_wpeView, [this](WPEBuffer* buffer) {
        return frameRendered(buffer);
    });
    setCursorHandler(m_wpeView, [this](const char* name) {
        cursorChanged(name);
    });
    g_signal_connect(m_wpeView, "buffers-changed", G_CALLBACK(+[](WPEView*, WPEBuffer** buffers, guint count, gpointer block) {
        static_cast<Block*>(block)->presentedBuffersChanged(buffers, count);
    }), this);
}

void Block::inspectorViewFinalized(gpointer block, GObject*)
{
    static_cast<Block*>(block)->inspectorViewGone();
}

void Block::openInspector(WPEView* view)
{
    m_isInspector = true;
    m_wpeView = view;
    attachView();
    // The view is the engine's: the block hears when the engine lets go of it.
    g_object_weak_ref(G_OBJECT(view), inspectorViewFinalized, this);
    g_signal_connect(view, "resized", G_CALLBACK(+[](WPEView*, gpointer block) {
        static_cast<Block*>(block)->inspectorResized();
    }), this);
}

void Block::inspectorResized()
{
    if (!m_wpeView || (wpe_view_get_width(m_wpeView) == static_cast<int>(m_width) && wpe_view_get_height(m_wpeView) == static_cast<int>(m_height)))
        return;
    // The engine sizes the inspector's window as it opens it: the client's size applies again, once the engine
    // is done.
    runOnMainLoop([weakThis = weak_from_this()] {
        if (auto block = weakThis.lock(); block && block->m_wpeView)
            resizeView(block->m_wpeView, static_cast<int>(block->m_width), static_cast<int>(block->m_height));
    });
}

void Block::inspectorViewGone()
{
    // Finalized: nothing is asked of the view again.
    m_wpeView = nullptr;
    runOnMainLoop([weakThis = weak_from_this(), &host = m_host, identifier = m_identifier] {
        if (weakThis.lock())
            host.endBlock(identifier);
    });
}

// The item the user chose of the menu a `contextMenu` event with `items` asked for.
void Block::chooseMenuItem(const Json& operation, const Reply& reply)
{
    auto item = operation.numberMember("item");
    if (!item || *item < 0 || *item > UINT32_MAX || *item != std::floor(*item)) {
        reply->invalid("menu: `item` is the `id` of an item of the menu asked for");
        return;
    }
    if (m_wpeView)
        wpe_view_context_menu_item_activated(m_wpeView, static_cast<guint>(*item));
    reply->ok();
}

// Opens the page's Web Inspector, whose page is a block of its own (openInspector()) that the client shows as it
// shows any block, and closes to close the inspector. Answered with that block, also when it is open already.
void Block::inspect(const Json&, const Reply& reply)
{
    auto answer = [&reply](const Block& inspector) {
        auto value = Json::object();
        value.setNumber("block", static_cast<double>(inspector.identifier()));
        reply->ok(value);
    };
    if (auto inspector = m_inspector.lock()) {
        answer(*inspector);
        return;
    }
    // The inspector reaches the page through the instrumentation developer extras turn on.
    webkit_settings_set_enable_developer_extras(m_settings.get(), TRUE);
    // The engine makes the inspector's view as it opens it, in a window of the display's.
    WPEView* made = nullptr;
    GRef<WPEToplevel> window;
    setViewCreatedHandler(m_host.display(), [&made, &window](WPEView* view) {
        made = view;
        window = GRef<WPEToplevel>::retain(wpe_view_get_toplevel(view));
    });
    webkit_web_view_toggle_inspector(m_view.get());
    setViewCreatedHandler(m_host.display(), nullptr);
    if (!made) {
        reply->failed("inspect: the Web Inspector did not open");
        return;
    }
    // The engine keeps the view out of its window until the inspector's page has loaded, and a view out of a
    // window renders nothing, which that page waits for: it goes back in at once.
    if (window && !wpe_view_get_toplevel(made))
        wpe_view_set_toplevel(made, window.get());
    auto inspector = m_host.adoptInspector(made);
    m_inspector = inspector;
    answer(*inspector);
}

void Block::close()
{
    if (std::exchange(m_closed, true))
        return;

    // A web process a script keeps busy would go on running it for a while after its page closed: it is ended.
    bool hung = m_view && (unresponsive() || (m_pingSent && g_get_monotonic_time() - *m_pingSent >= pingInterval * G_TIME_SPAN_MILLISECOND));
    stopWatchingResponsiveness();
    m_pageCalls.clear();

    // Nothing held for the page outlives it.
    if (auto* dialog = std::exchange(m_heldDialog, nullptr)) {
        webkit_script_dialog_close(dialog);
        webkit_script_dialog_unref(dialog);
    }
    if (auto chooser = std::exchange(m_heldChooser, { }))
        webkit_file_chooser_request_cancel(chooser.get());
    for (auto& resource : std::exchange(m_resources, { }))
        g_signal_handlers_disconnect_by_data(resource.resource.get(), this);
    for (auto& download : std::exchange(m_downloads, { })) {
        g_signal_handlers_disconnect_by_data(download.download.get(), this);
        webkit_download_cancel(download.download.get());
    }
    m_frameCalls.clear();
    failEvaluations(std::exchange(m_evaluations, { }), "eval: the browser block closed");
    if (auto waiter = std::exchange(m_eventsWaiter, nullptr))
        waiter->notFound("no browser block " + std::to_string(m_identifier));
    stopPresenting(true);
    m_frameWaiter = nullptr;
    if (auto capture = std::exchange(m_capture, nullptr))
        cancelCaptureTimer(*capture);
    m_captures.clear();

    if (m_session)
        g_signal_handlers_disconnect_by_data(m_session.get(), this);
    if (m_contentManager) {
        g_signal_handlers_disconnect_by_data(m_contentManager.get(), this);
        for (auto* script : std::exchange(m_scripts, { }))
            webkit_user_script_unref(script);
        if (auto* filter = std::exchange(m_allowFilter, nullptr))
            webkit_user_content_filter_unref(filter);
    }
    if (m_isInspector && m_wpeView) {
        g_object_weak_unref(G_OBJECT(m_wpeView), inspectorViewFinalized, this);
        setFrameHandler(m_wpeView, nullptr);
        setCursorHandler(m_wpeView, nullptr);
        g_signal_handlers_disconnect_by_data(m_wpeView, this);
        // Closed by the client: the engine closes the inspector, as its window's close button would.
        wpe_view_closed(m_wpeView);
    }
    if (m_view) {
        g_signal_handlers_disconnect_by_data(m_view.get(), this);
        g_signal_handlers_disconnect_by_data(webkit_web_view_get_back_forward_list(m_view.get()), this);
        setFrameHandler(m_wpeView, nullptr);
        setCursorHandler(m_wpeView, nullptr);
        g_signal_handlers_disconnect_by_data(m_wpeView, this);
        webkit_web_view_stop_loading(m_view.get());
        if (hung)
            webkit_web_view_terminate_web_process(m_view.get());
    }
    m_wpeView = nullptr;
    m_view.clear();
    m_contentManager.clear();
    m_settings.clear();
    m_session.clear();
}

std::string Block::url() const
{
    const char* uri = m_view ? webkit_web_view_get_uri(m_view.get()) : nullptr;
    return uri && *uri ? uri : "about:blank";
}

std::string Block::title() const
{
    return stringOrEmpty(m_view ? webkit_web_view_get_title(m_view.get()) : nullptr);
}

bool Block::handle(const std::string& name, const Json& operation, const Reply& reply)
{
    using Handler = void (Block::*)(const Json&, const Reply&);
    static const std::map<std::string, Handler> handlers = {
        { "state", &Block::state },
        { "goto", &Block::navigate },
        { "nav", &Block::moveInHistory },
        { "viewport", &Block::setViewport },
        { "events", &Block::events },
        { "dialogs", &Block::setDialogPolicy },
        { "dialog", &Block::answerDialog },
        { "scripts", &Block::setScripts },
        { "eval", &Block::evaluate },
        { "input", &Block::input },
        { "edit", &Block::edit },
        { "capture", &Block::capture },
        { "cookies", &Block::cookies },
        { "setCookie", &Block::setCookie },
        { "deleteCookie", &Block::deleteCookie },
        { "agent", &Block::setAgent },
        { "appearance", &Block::setAppearance },
        { "allow", &Block::setAllowedHosts },
        { "insecure", &Block::setInsecure },
        { "fileAccess", &Block::setFileAccess },
        { "credentials", &Block::setCredentials },
        { "downloads", &Block::setDownloads },
        { "files", &Block::setFiles },
        { "dom", &Block::query },
        { "identity", &Block::setIdentity },
        { "present", &Block::present },
        { "focus", &Block::setFocus },
        { "inspect", &Block::inspect },
        { "menu", &Block::chooseMenuItem },
        { "pdf", &Block::unsupported },
        { "media", &Block::unsupported },
    };
    // A Web Inspector's page is the engine's: it is shown and used, not driven.
    static const char* const inspectorOperations[] = { "viewport", "events", "input", "present", "focus", "menu" };
    auto found = handlers.find(name);
    if (found == handlers.end())
        return false;
    if (m_closed) {
        reply->notFound("no browser block " + std::to_string(m_identifier));
        return true;
    }
    if (m_isInspector && std::ranges::find(inspectorOperations, name) == std::end(inspectorOperations)) {
        reply->unsupported(name + ": a Web Inspector's block is shown, sized, focused and given input only");
        return true;
    }
    (this->*found->second)(operation, reply);
    return true;
}

void Block::unsupported(const Json& operation, const Reply& reply)
{
    if (operation.stringMember("op").value_or("") == "pdf")
        reply->unsupported("pdf: WPE WebKit cannot print");
    else
        reply->unsupported("media: WPE WebKit has no CSS media type override");
}

// Events.

void Block::record(Json&& event)
{
    event.setNumber("seq", static_cast<double>(m_nextSequence++));
    m_events.push_back(event.serialize());
    if (m_events.size() > ringSize)
        m_events.pop_front();
    if (auto waiter = std::exchange(m_eventsWaiter, nullptr))
        answerEvents(waiter, m_eventsWaiterAfter);
}

void Block::events(const Json& operation, const Reply& reply)
{
    uint64_t after = 0;
    if (auto member = operation.get("after"); member && !member.isNull()) {
        double number = member.isNumber() ? member.toNumber() : -1;
        if (number < 0 || number != std::floor(number)) {
            reply->invalid("events: `after` is the last sequence number seen");
            return;
        }
        after = number >= 18446744073709551615.0 ? UINT64_MAX : static_cast<uint64_t>(number);
    }
    bool wait = false;
    if (auto member = operation.get("wait"); member && !member.isNull()) {
        if (!member.isBoolean()) {
            reply->invalid("events: `wait` must be true or false");
            return;
        }
        wait = member.toBoolean();
    }
    // One waiting call at a time: the one before answers with what it has.
    if (auto waiter = std::exchange(m_eventsWaiter, nullptr))
        answerEvents(waiter, m_eventsWaiterAfter);
    if (wait && (after == UINT64_MAX || after + 1 >= m_nextSequence)) {
        reply->setUnansweredMessage("events: the browser block closed");
        m_eventsWaiter = reply;
        m_eventsWaiterAfter = after;
        return;
    }
    answerEvents(reply, after);
}

void Block::answerEvents(const Reply& reply, uint64_t after)
{
    uint64_t oldest = m_nextSequence - m_events.size();
    uint64_t dropped = after == UINT64_MAX || oldest <= after + 1 ? 0 : oldest - (after + 1);

    std::string answer = "{\"events\":[";
    bool first = true;
    for (size_t i = after >= oldest ? std::min<uint64_t>(after - oldest + 1, m_events.size()) : 0; i < m_events.size(); i++) {
        if (!std::exchange(first, false))
            answer += ',';
        answer += m_events[i];
    }
    answer += "],\"next\":" + std::to_string(m_nextSequence - 1) + ",\"dropped\":" + std::to_string(dropped) + "}";
    reply->ok(answer);
}

void Block::reportHistory()
{
    std::pair<bool, bool> history { webkit_web_view_can_go_back(m_view.get()), webkit_web_view_can_go_forward(m_view.get()) };
    if (m_reportedHistory == history)
        return;
    m_reportedHistory = history;
    auto event = eventOfType("history");
    event.setBoolean("back", history.first);
    event.setBoolean("forward", history.second);
    record(std::move(event));
}

void Block::contextMenu(WebKitHitTestResult* hit)
{
    auto event = eventOfType("contextMenu");
    if (m_pointer) {
        event.setNumber("x", m_pointer->first);
        event.setNumber("y", m_pointer->second);
    }
    auto uri = [&](const char* name, bool is, const char* value) {
        if (is && value && *value)
            event.setString(name, value);
    };
    uri("link", webkit_hit_test_result_context_is_link(hit), webkit_hit_test_result_get_link_uri(hit));
    uri("image", webkit_hit_test_result_context_is_image(hit), webkit_hit_test_result_get_image_uri(hit));
    uri("media", webkit_hit_test_result_context_is_media(hit), webkit_hit_test_result_get_media_uri(hit));
    event.setBoolean("editable", webkit_hit_test_result_context_is_editable(hit));
    event.setBoolean("selection", webkit_hit_test_result_context_is_selection(hit));
    event.setBoolean("back", webkit_web_view_can_go_back(m_view.get()));
    event.setBoolean("forward", webkit_web_view_can_go_forward(m_view.get()));
    record(std::move(event));
}

void Block::connectSignals()
{
    auto* view = m_view.get();
    g_signal_connect(view, "notify::uri", G_CALLBACK(+[](WebKitWebView*, GParamSpec*, gpointer block) {
        static_cast<Block*>(block)->uriChanged();
    }), this);
    g_signal_connect(view, "notify::title", G_CALLBACK(+[](WebKitWebView*, GParamSpec*, gpointer block) {
        static_cast<Block*>(block)->titleChanged();
    }), this);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(+[](WebKitWebView*, GParamSpec*, gpointer block) {
        static_cast<Block*>(block)->progressChanged();
    }), this);
    g_signal_connect(view, "load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer block) {
        static_cast<Block*>(block)->loadChanged(event);
    }), this);
    g_signal_connect(view, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const char* uri, GError* error, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->loadFailed(uri, error);
    }), this);
    g_signal_connect(view, "load-failed-with-tls-errors", G_CALLBACK(+[](WebKitWebView*, const char* uri, GTlsCertificate* certificate, GTlsCertificateFlags, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->loadFailedWithTLSErrors(uri, certificate);
    }), this);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer block) {
        static_cast<Block*>(block)->webProcessTerminated(reason);
    }), this);
    g_signal_connect(view, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->decidePolicy(decision, type);
    }), this);
    // A page asking for a new window gets none: the request is reported.
    g_signal_connect(view, "create", G_CALLBACK(+[](WebKitWebView*, WebKitNavigationAction* action, gpointer block) -> WebKitWebView* {
        auto* request = webkit_navigation_action_get_request(action);
        auto event = eventOfType("newWindow");
        event.setString("url", stringOrEmpty(request ? webkit_uri_request_get_uri(request) : nullptr));
        static_cast<Block*>(block)->record(std::move(event));
        return nullptr;
    }), this);
    // The page left a context click (or the Menu key) to the engine, which has no menu to show: what is under the
    // pointer is reported for the client to show one.
    g_signal_connect(view, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu* menu, WebKitHitTestResult* hit, gpointer block) -> gboolean {
        // A page whose contextmenu listener called preventDefault() still gets this signal once an
        // earlier click made a menu: an empty one, about what that earlier click hit.
        if (webkit_context_menu_get_n_items(menu))
            static_cast<Block*>(block)->contextMenu(hit);
        return TRUE;
    }), this);
    g_signal_connect(view, "script-dialog", G_CALLBACK(+[](WebKitWebView*, WebKitScriptDialog* dialog, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->scriptDialog(dialog);
    }), this);
    g_signal_connect(view, "run-file-chooser", G_CALLBACK(+[](WebKitWebView*, WebKitFileChooserRequest* request, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->runFileChooser(request);
    }), this);
    g_signal_connect(view, "authenticate", G_CALLBACK(+[](WebKitWebView*, WebKitAuthenticationRequest* request, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->authenticate(request);
    }), this);
    g_signal_connect(view, "user-message-received", G_CALLBACK(+[](WebKitWebView*, WebKitUserMessage* message, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->userMessageReceived(message);
    }), this);
    g_signal_connect(view, "resource-load-started", G_CALLBACK(+[](WebKitWebView*, WebKitWebResource* resource, WebKitURIRequest* request, gpointer block) {
        static_cast<Block*>(block)->resourceLoadStarted(resource, request);
    }), this);
    g_signal_connect(webkit_web_view_get_back_forward_list(view), "changed", G_CALLBACK(+[](WebKitBackForwardList*, WebKitBackForwardListItem*, gpointer, gpointer block) {
        static_cast<Block*>(block)->reportHistory();
    }), this);
    g_signal_connect(m_session.get(), "download-started", G_CALLBACK(+[](WebKitNetworkSession*, WebKitDownload* download, gpointer block) {
        static_cast<Block*>(block)->downloadStarted(download);
    }), this);
}

void Block::uriChanged()
{
    const char* uri = webkit_web_view_get_uri(m_view.get());
    if (!uri || !*uri)
        return;
    auto event = eventOfType("url");
    event.setString("url", uri);
    record(std::move(event));
}

void Block::titleChanged()
{
    auto event = eventOfType("title");
    event.setString("title", title());
    record(std::move(event));
}

void Block::progressChanged()
{
    double progress = webkit_web_view_get_estimated_load_progress(m_view.get());
    // Below 1 outside a load, it is the reset that follows a web process's death, not a load starting.
    if (progress < 1 && !webkit_web_view_is_loading(m_view.get()))
        return;
    auto event = eventOfType("progress");
    event.setNumber("value", progress);
    record(std::move(event));
}

void Block::loadChanged(WebKitLoadEvent loadEvent)
{
    std::string url = stringOrEmpty(webkit_web_view_get_uri(m_view.get()));
    switch (loadEvent) {
    case WEBKIT_LOAD_STARTED:
        m_loadFailed = false;
        // The page took the navigation up.
        m_pendingNavigation = nullptr;
        // The engine loads its error page for the address that failed: that is no page of the site's.
        m_errorPageShown = std::exchange(m_errorPage, std::nullopt) == url;
        if (!m_errorPageShown)
            m_error = std::nullopt;
        break;
    case WEBKIT_LOAD_REDIRECTED:
        break;
    case WEBKIT_LOAD_COMMITTED: {
        documentReplaced();
        // A document committed, so the page's process answers: a round trip still out may have gone to a process
        // the navigation swapped out, which never answers it.
        if (m_pingSent)
            pingAnswered(m_pingGeneration);
        if (m_errorPageShown)
            break;
        auto event = eventOfType("committed");
        event.setString("url", url);
        record(std::move(event));
        break;
    }
    case WEBKIT_LOAD_FINISHED: {
        bool errorPage = std::exchange(m_errorPageShown, false);
        bool failed = std::exchange(m_loadFailed, false);
        if (!failed)
            m_tlsAccepted.clear();
        if (!errorPage && !failed) {
            auto event = eventOfType("loaded");
            event.setString("url", url);
            record(std::move(event));
        }
        auto progress = eventOfType("progress");
        progress.setNumber("value", 1);
        record(std::move(progress));
        reportHistory();
        break;
    }
    }
}

bool Block::loadFailed(const char* uri, GError* error)
{
    // A video or audio file opened as the page fails its document load this way, then
    // loads again into the media document that plays it: nothing failed.
    if (error && g_error_matches(error, WEBKIT_MEDIA_ERROR, WEBKIT_MEDIA_ERROR_WILL_HANDLE_LOAD))
        return false;
    bool cancelled = error && g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED);
    bool interrupted = error && g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE);
    m_loadFailed = true;
    // A load that was cancelled, became a download or was dropped by a policy shows no error page,
    // and is no failure: the page it would have replaced stays (a download reports itself).
    m_errorPage = cancelled || interrupted || !uri ? std::nullopt : std::optional<std::string>(uri);
    if (error && !cancelled && !interrupted) {
        m_error = stringOrEmpty(error->message);
        auto event = eventOfType("failed");
        event.setString("message", *m_error);
        record(std::move(event));
    }
    return false;
}

void Block::webProcessTerminated(WebKitWebProcessTerminationReason reason)
{
    if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API)
        return;
    stopWatchingResponsiveness();
    m_frames.clear();
    m_frameCalls.clear();
    failEvaluations(std::exchange(m_evaluations, { }), "eval: the page's web process is gone");
#if defined(_WIN32)
    // A web process that crashed in the graphics stack took the GPU with it, not the page: the page comes back,
    // drawn in software, once. (Reloaded after the engine is done telling of the crash.)
    if (reason == WEBKIT_WEB_PROCESS_CRASHED && m_settings && m_drawsWithGPU && !m_host.drawsWithGPU()) {
        drawWithoutGPU();
        runAfter(0, [weakThis = weak_from_this()] {
            auto block = weakThis.lock();
            if (!block || block->m_closed)
                return;
            // The page that was showing comes back from history; one the process went with before it showed
            // loads again.
            auto* view = block->m_view.get();
            const char* uri = webkit_web_view_get_uri(view);
            if (webkit_back_forward_list_get_current_item(webkit_web_view_get_back_forward_list(view)))
                webkit_web_view_reload(view);
            else if (uri && *uri && strcmp(uri, "about:blank"))
                webkit_web_view_load_uri(view, uri);
        });
        return;
    }
#endif
    m_error = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "The page's web process went over its memory limit" : "The page's web process crashed";
    auto event = eventOfType("failed");
    event.setString("message", *m_error);
    // The page that was showing went with its process, unlike a load that failed.
    event.setBoolean("gone", true);
    record(std::move(event));
}

// Navigation and state.

void Block::state(const Json&, const Reply& reply)
{
    auto* view = m_view.get();
    bool loading = webkit_web_view_is_loading(view);
    auto answer = Json::object();
    answer.setString("url", url());
    answer.setString("title", title());
    answer.setBoolean("loading", loading);
    answer.setNumber("progress", loading ? webkit_web_view_get_estimated_load_progress(view) : 1);
    answer.setBoolean("back", webkit_web_view_can_go_back(view));
    answer.setBoolean("forward", webkit_web_view_can_go_forward(view));
    answer.setNumber("width", m_width);
    answer.setNumber("height", m_height);
    // On a screen only while presented; the focused page of an active window to its scripts unless `focus` said not.
    answer.setBoolean("shown", presented());
    answer.setBoolean("focused", m_focused);
    // Whether the page's web process answers: a script that keeps it busy holds up every call into the page.
    answer.setBoolean("responsive", !unresponsive());
    if (m_error)
        answer.setString("error", *m_error);
    else
        answer.setNull("error");
    reply->ok(answer);
}

void Block::navigate(const Json& operation, const Reply& reply)
{
    auto address = operation.stringMember("url");
    if (!address || address->empty()) {
        reply->invalid("goto: `url` is the address to load");
        return;
    }
    // A page already showing the address is reloaded. (While a load is under way, the view's address is that load's.)
    const char* current = webkit_web_view_get_uri(m_view.get());
    bool reload = current && *address == current && !webkit_web_view_is_loading(m_view.get());
    startNavigation([this, reload, address = *address] {
        if (reload)
            webkit_web_view_reload(m_view.get());
        else
            webkit_web_view_load_uri(m_view.get(), address.c_str());
    });
    reply->ok();
}

void Block::moveInHistory(const Json& operation, const Reply& reply)
{
    auto go = operation.stringMember("go").value_or("");
    if (go == "back")
        startNavigation([this] { webkit_web_view_go_back(m_view.get()); });
    else if (go == "forward")
        startNavigation([this] { webkit_web_view_go_forward(m_view.get()); });
    else if (go == "reload")
        startNavigation([this] { webkit_web_view_reload(m_view.get()); });
    else if (go == "stop")
        webkit_web_view_stop_loading(m_view.get());
    else {
        reply->invalid("nav: `go` is back, forward, reload or stop");
        return;
    }
    reply->ok();
}

void Block::setViewport(const Json& operation, const Reply& reply)
{
    auto width = operation.numberMember("width");
    auto height = operation.numberMember("height");
    if (!width || !height) {
        reply->invalid("viewport: `width` and `height` are the page's CSS px");
        return;
    }
    double roundedWidth = std::round(*width);
    double roundedHeight = std::round(*height);
    if (!(roundedWidth >= 1 && roundedWidth <= 16384)) {
        reply->invalid("`width` must be 1 to 16384 CSS px");
        return;
    }
    if (!(roundedHeight >= 1 && roundedHeight <= 16384)) {
        reply->invalid("`height` must be 1 to 16384 CSS px");
        return;
    }
    double scale = m_scale;
    if (!readScale(operation, scale, reply))
        return;
    if (scale != m_scale) {
        m_scale = scale;
        setViewScale(m_wpeView, m_scale);
    }
    m_width = static_cast<unsigned>(roundedWidth);
    m_height = static_cast<unsigned>(roundedHeight);
    // A capture of the whole document has the view at another size for a moment, and puts this one back.
    if (!m_capture)
        resizeView(m_wpeView, static_cast<int>(m_width), static_cast<int>(m_height));
    applyIdentity();
    reply->ok();
}

// Whether the view holds the keyboard. Without it the page is no longer told it is focused whatever happens.
void Block::setFocus(const Json& operation, const Reply& reply)
{
    auto value = operation.get("value");
    if (!value || !value.isBoolean()) {
        reply->invalid("focus: `value` is true or false");
        return;
    }
    m_focused = value.toBoolean();
    setFeature("FocusEmulation", m_focused);
    if (m_focused)
        wpe_view_focus_in(m_wpeView);
    else
        wpe_view_focus_out(m_wpeView);
    reply->ok();
}

bool Block::decidePolicy(WebKitPolicyDecision* decision, WebKitPolicyDecisionType type)
{
    switch (type) {
    case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
        return decideNavigation(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
        return decideResponse(WEBKIT_RESPONSE_POLICY_DECISION(decision));
    default:
        return false;
    }
}

// Responsiveness.

bool Block::admitPageCall(const Json& operation, const Reply& reply)
{
    auto name = operation.stringMember("op").value_or("call");
    bool failWhenUnresponsive = operation.booleanMember("failUnresponsive").value_or(false);
    if (failWhenUnresponsive && unresponsive()) {
        reply->fail("unresponsive", unresponsiveMessage(name));
        return false;
    }
    std::erase_if(m_pageCalls, [](const PageCall& call) {
        auto reply = call.reply.lock();
        return !reply || reply->answered();
    });
    m_pageCalls.push_back({ std::move(name), reply, failWhenUnresponsive });
    ping();
    return true;
}

void Block::ping()
{
    if (m_closed || !m_view || m_pingSent)
        return;
    if (auto timer = std::exchange(m_nextPing, 0))
        g_source_remove(timer);
    m_pingSent = g_get_monotonic_time();
    uint64_t generation = ++m_pingGeneration;
    m_pingTimer = runAfter(unresponsivePatience, [weakThis = weak_from_this()] {
        if (auto block = weakThis.lock()) {
            block->m_pingTimer = 0;
            block->pingOverdue();
        }
    });
    // Every message to the page waits for its main thread: an empty function answers as soon as that is free.
    run("", "[]", World::Isolated, [weakThis = weak_from_this(), generation](std::optional<std::string>&&, GError*) {
        if (auto block = weakThis.lock())
            block->pingAnswered(generation);
    });
}

void Block::pingOverdue()
{
    if (m_closed || !m_pingSent)
        return;
    // A held dialog stops the page until it is answered, which is no busy script: the round trip waits on.
    if (m_heldDialog) {
        m_pingTimer = runAfter(pingInterval, [weakThis = weak_from_this()] {
            if (auto block = weakThis.lock()) {
                block->m_pingTimer = 0;
                block->pingOverdue();
            }
        });
        return;
    }
    if (!std::exchange(m_unresponsive, true)) {
        g_warning("block %" G_GUINT64_FORMAT ": the page is unresponsive", m_identifier);
        record(timedEventOfType("unresponsive"));
    }
    failPageCalls(false);
    // A navigation the page never took up starts again in a new web process.
    if (auto navigate = std::exchange(m_pendingNavigation, nullptr)) {
        endUnresponsiveProcess();
        navigate();
    }
}

void Block::pingAnswered(uint64_t generation)
{
    if (m_closed || generation != m_pingGeneration)
        return;
    m_pingSent = std::nullopt;
    if (auto timer = std::exchange(m_pingTimer, 0))
        g_source_remove(timer);
    if (std::exchange(m_unresponsive, false))
        record(timedEventOfType("responsive"));
    // The page took up every message sent before the round trip: a navigation sent before it too.
    if (generation > m_pendingNavigationAfter)
        m_pendingNavigation = nullptr;
    std::erase_if(m_pageCalls, [](const PageCall& call) {
        auto reply = call.reply.lock();
        return !reply || reply->answered();
    });
    if (m_pageCalls.empty() && !m_pendingNavigation)
        return;
    m_nextPing = runAfter(m_pendingNavigation ? 0 : pingInterval, [weakThis = weak_from_this()] {
        if (auto block = weakThis.lock()) {
            block->m_nextPing = 0;
            block->ping();
        }
    });
}

bool Block::unresponsive() const
{
    return m_unresponsive || (m_view && !webkit_web_view_get_is_web_process_responsive(m_view.get()));
}

std::string Block::unresponsiveMessage(const std::string& operation, const char* ending) const
{
    std::string message = operation + ": the page is unresponsive: a script keeps its web process busy";
    if (m_pingSent) {
        GChars since(g_strdup_printf(" (no answer for %.1f s)", static_cast<double>(g_get_monotonic_time() - *m_pingSent) / G_TIME_SPAN_SECOND));
        message += since.get();
    }
    if (ending)
        message.append("; ").append(ending);
    return message;
}

void Block::failPageCalls(bool all, const char* ending)
{
    for (auto& call : std::exchange(m_pageCalls, { })) {
        auto reply = call.reply.lock();
        if (!reply || reply->answered())
            continue;
        if (all || call.failWhenUnresponsive)
            reply->fail("unresponsive", unresponsiveMessage(call.operation, ending));
        else
            m_pageCalls.push_back(std::move(call));
    }
    releaseFailedCapture();
}

void Block::startNavigation(std::function<void()>&& navigate)
{
    m_tlsAccepted.clear();
    // The client's navigation is no error page the engine loads for the last failure, even to the same address.
    m_errorPage = std::nullopt;
    // A page that answers nothing would not take the navigation up either: it goes to a new web process.
    if (unresponsive()) {
        endUnresponsiveProcess();
        navigate();
        return;
    }
    navigate();
    // Should the page turn out not to answer before it took the navigation up, the navigation starts again then.
    m_pendingNavigation = std::move(navigate);
    m_pendingNavigationAfter = m_pingGeneration;
    ping();
}

void Block::endUnresponsiveProcess()
{
    g_warning("block %" G_GUINT64_FORMAT ": ending the unresponsive page's web process", m_identifier);
    // Every evaluation, DOM read and capture is a page call: they fail here, and what the gone process would
    // have answered is forgotten.
    failPageCalls(true, "its web process was ended");
    m_frames.clear();
    m_frameCalls.clear();
    m_evaluations.clear();
    stopWatchingResponsiveness();
    // The engine reports the termination it was asked for as no crash (webProcessTerminated()).
    webkit_web_view_terminate_web_process(m_view.get());
}

void Block::stopWatchingResponsiveness()
{
    m_pingSent = std::nullopt;
    // A round trip still in flight answers for a process that is gone.
    ++m_pingGeneration;
    if (auto timer = std::exchange(m_pingTimer, 0))
        g_source_remove(timer);
    if (auto timer = std::exchange(m_nextPing, 0))
        g_source_remove(timer);
    m_unresponsive = false;
    m_pendingNavigation = nullptr;
}

Json Block::eventOfType(const char* type)
{
    auto event = Json::object();
    event.setString("type", type);
    return event;
}

Json Block::timedEventOfType(const char* type)
{
    auto event = eventOfType(type);
    event.setNumber("ts", static_cast<double>(g_get_real_time() / 1000));
    return event;
}

} // namespace AgentHost
