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
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AgentHost {

const char isolatedWorldName[] = "ABHost";

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

void Block::open(const std::string& url)
{
    m_settings = GRef<WebKitSettings>::adopt(webkit_settings_new());
    // A remote inspector reaches a page only through the instrumentation developer extras turn on.
    if (m_host.inspectorEnabled())
        webkit_settings_set_enable_developer_extras(m_settings.get(), TRUE);
    // The page is always focused in an active window, and renders no frame nobody asked for.
    setFeature("FocusEmulation", true);
    setFeature("CompositionSuppressed", true);
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
    setFrameHandler(m_wpeView, [this](WPEBuffer* buffer) {
        return frameRendered(buffer);
    });
    setCursorHandler(m_wpeView, [this](const char* name) {
        cursorChanged(name);
    });
    g_signal_connect(m_wpeView, "buffers-changed", G_CALLBACK(+[](WPEView*, WPEBuffer** buffers, guint count, gpointer block) {
        static_cast<Block*>(block)->presentedBuffersChanged(buffers, count);
    }), this);
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

void Block::close()
{
    if (std::exchange(m_closed, true))
        return;

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
    if (m_view) {
        g_signal_handlers_disconnect_by_data(m_view.get(), this);
        g_signal_handlers_disconnect_by_data(webkit_web_view_get_back_forward_list(m_view.get()), this);
        setFrameHandler(m_wpeView, nullptr);
        setCursorHandler(m_wpeView, nullptr);
        g_signal_handlers_disconnect_by_data(m_wpeView, this);
        webkit_web_view_stop_loading(m_view.get());
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
        { "pdf", &Block::unsupported },
        { "media", &Block::unsupported },
    };
    auto found = handlers.find(name);
    if (found == handlers.end())
        return false;
    if (m_closed) {
        reply->notFound("no browser block " + std::to_string(m_identifier));
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
        // The engine loads its error page for the address that failed: that is no page of the site's.
        m_errorPageShown = std::exchange(m_errorPage, std::nullopt) == url;
        if (!m_errorPageShown)
            m_error = std::nullopt;
        break;
    case WEBKIT_LOAD_REDIRECTED:
        break;
    case WEBKIT_LOAD_COMMITTED: {
        documentReplaced();
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
    bool cancelled = error && g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED);
    bool interrupted = error && g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE);
    m_loadFailed = true;
    // A load that was cancelled, became a download or was dropped by a policy shows no error page.
    m_errorPage = cancelled || interrupted || !uri ? std::nullopt : std::optional<std::string>(uri);
    if (error && !cancelled) {
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
    m_error = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "The page's web process went over its memory limit" : "The page's web process crashed";
    m_frames.clear();
    m_frameCalls.clear();
    failEvaluations(std::exchange(m_evaluations, { }), "eval: the page's web process is gone");
    auto event = eventOfType("failed");
    event.setString("message", *m_error);
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
    if (current && *address == current && !webkit_web_view_is_loading(m_view.get()))
        webkit_web_view_reload(m_view.get());
    else
        webkit_web_view_load_uri(m_view.get(), address->c_str());
    reply->ok();
}

void Block::moveInHistory(const Json& operation, const Reply& reply)
{
    auto go = operation.stringMember("go").value_or("");
    if (go == "back")
        webkit_web_view_go_back(m_view.get());
    else if (go == "forward")
        webkit_web_view_go_forward(m_view.get());
    else if (go == "reload")
        webkit_web_view_reload(m_view.get());
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
