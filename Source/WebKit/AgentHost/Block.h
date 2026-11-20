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

// A block: one web view, what the protocol set on it, and the events it
// reported. Block.cpp holds its life, navigation and events;
// BlockSettings.cpp what the protocol sets on it; BlockNetwork.cpp what it
// sees of its pages' loads, consoles and downloads; BlockScripts.cpp what
// goes on between the host and the pages' scripts (evaluation, user scripts,
// messages, frames); BlockPage.cpp dialogs, file choosers, editing commands,
// cookies and DOM reads; BlockInput.cpp trusted input; BlockCapture.cpp
// images; BlockPresent.cpp frames shown by a client.

#include "Host.h"
#include "Image.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace AgentHost {

// The script world of the host's own scripts and of `isolated` scripts and evaluations.
extern const char isolatedWorldName[];

class Block : public std::enable_shared_from_this<Block> {
public:
    Block(Host&, uint64_t identifier, unsigned width, unsigned height, double scale, GRef<WebKitNetworkSession>&&);
    ~Block();

    // Makes the web view and starts loading `url` in it.
    void open(const std::string& url);
    // Makes the block a page's Web Inspector, whose page is in `view`, a view the engine made. It is shown, sized,
    // focused and fed input as any block is; closing it closes the inspector, and it closes with the inspector
    // (when the inspected block does).
    void openInspector(WPEView*);
    // Lets go of the web view and of everything held for the page.
    void close();

    // Runs block operation `name`. Returns false for a name that is no operation.
    bool handle(const std::string& name, const Json& operation, const Reply&);

    uint64_t identifier() const { return m_identifier; }
    std::string url() const;
    std::string title() const;

    // Whether the block's frames are presented to a client.
    bool presented() const { return !!m_presentation; }
    // The client no longer shows a buffer of the block's.
    void presentedBufferReleased(uint32_t buffer);
    // A client died: the block is no longer presented to it.
    void presentClientGone(const PresentClient&);

private:
    enum class World { Page, Isolated };
    enum class DialogPolicy { Default, Accept, Dismiss };

    struct Download {
        GRef<WebKitDownload> download;
        uint64_t identifier { 0 };
        std::string url;
        std::optional<std::string> path;
    };

    // A resource a page of the block loads, from the start of its load to its end.
    struct Resource {
        GRef<WebKitWebResource> resource;
        uint64_t identifier { 0 };
        // The main frame's document.
        bool main { false };
        bool responded { false };
        bool failed { false };
        std::string url;
    };

    // What `identity` says of the screen and of the window (CSS px).
    struct ScreenProfile {
        unsigned width { 0 };
        unsigned height { 0 };
        int availLeft { 0 };
        int availTop { 0 };
        unsigned availWidth { 0 };
        unsigned availHeight { 0 };
    };
    struct WindowProfile {
        int x { 0 };
        int y { 0 };
        unsigned width { 0 };
        unsigned height { 0 };
    };

    struct InputStep;
    struct InputPlayer;
    struct CaptureRequest;

    // Block.cpp
    void state(const Json&, const Reply&);
    void navigate(const Json&, const Reply&);
    void moveInHistory(const Json&, const Reply&);
    void setViewport(const Json&, const Reply&);
    void events(const Json&, const Reply&);
    void answerEvents(const Reply&, uint64_t after);
    void setFocus(const Json&, const Reply&);
    void unsupported(const Json&, const Reply&);
    void record(Json&& event);
    static Json eventOfType(const char* type);
    // An event of the console or of a resource load: with the time it was observed (milliseconds since the epoch).
    static Json timedEventOfType(const char* type);
    void reportHistory();
    void contextMenu(WebKitHitTestResult*);
    // Has the view's frames, cursor and buffers come to the block.
    void attachView();
    void inspect(const Json&, const Reply&);
    void chooseMenuItem(const Json&, const Reply&);
    // The inspector's view changed size: when the engine sized it, the block's size applies again.
    void inspectorResized();
    // The engine let go of the inspector's view: the inspector closed.
    void inspectorViewGone();
    static void inspectorViewFinalized(gpointer block, GObject*);
    void setFeature(const char* identifier, bool enabled);
#if defined(_WIN32)
    // Has the page drawn in software (the HardwareAcceleration feature off) from its next web process on.
    void drawWithoutGPU();
#endif
    void connectSignals();
    void uriChanged();
    void titleChanged();
    void progressChanged();
    void loadChanged(WebKitLoadEvent);
    bool loadFailed(const char* uri, GError*);
    void webProcessTerminated(WebKitWebProcessTerminationReason);
    bool decidePolicy(WebKitPolicyDecision*, WebKitPolicyDecisionType);

    // Block.cpp: responsiveness. A script that keeps the page's web process busy holds up every call into the page.
    // While calls are pending the host makes round trips to the process; one unanswered for long makes the page
    // unresponsive. Starts the call (returning true) or fails it at once (`unresponsive`): the call asked to
    // fail (`failUnresponsive`) and the page is unresponsive now.
    bool admitPageCall(const Json& operation, const Reply&);
    void ping();
    void pingOverdue();
    void pingAnswered(uint64_t generation);
    // Whether the page is unresponsive, as the host's round trips or the engine found.
    bool unresponsive() const;
    std::string unresponsiveMessage(const std::string& operation, const char* ending = nullptr) const;
    // Fails the pending calls that asked to (all of them when `all`) with `unresponsive`.
    void failPageCalls(bool all, const char* ending = nullptr);
    // Starts a navigation in a page that takes it up: one that is unresponsive has its web process ended first.
    void startNavigation(std::function<void()>&&);
    // Ends the unresponsive page's web process; every call into the page fails.
    void endUnresponsiveProcess();
    void stopWatchingResponsiveness();

    // BlockSettings.cpp
    void setAgent(const Json&, const Reply&);
    void setAppearance(const Json&, const Reply&);
    void setIdentity(const Json&, const Reply&);
    void setInsecure(const Json&, const Reply&);
    void setFileAccess(const Json&, const Reply&);
    void setMuted(const Json&, const Reply&);
    void setCredentials(const Json&, const Reply&);
    void setDownloads(const Json&, const Reply&);
    void setAllowedHosts(const Json&, const Reply&);
    void applyDefaultIdentity();
    void applyDefaultUserAgent();
    void applyIdentity();
    bool decideNavigation(WebKitNavigationPolicyDecision*);
    bool loadFailedWithTLSErrors(const char* uri, GTlsCertificate*);
    bool authenticate(WebKitAuthenticationRequest*);

    // BlockNetwork.cpp
    bool decideResponse(WebKitResponsePolicyDecision*);
    void resourceLoadStarted(WebKitWebResource*, WebKitURIRequest*);
    void reportRequest(const Resource&, WebKitURIRequest*);
    void resourceResponded(Resource&, WebKitURIResponse*);
    void resourceFailed(WebKitWebResource*, const char* error);
    void resourceFinished(WebKitWebResource*);
    Resource* findResource(WebKitWebResource*);
    bool userMessageReceived(WebKitUserMessage*);
    void downloadStarted(WebKitDownload*);
    bool decideDestination(WebKitDownload*, const char* suggestedName);
    void downloadEnded(WebKitDownload*, const char* error);

    // BlockScripts.cpp
    void evaluate(const Json&, const Reply&);
    void setScripts(const Json&, const Reply&);
    void installHostScripts();
    void setPageWorldHandlers(bool);
    // Runs async function `body` in the main frame, its argument `__stencilArgs` holding `arguments` (JSON text).
    // `then` gets the result when it is a string, nothing when it is something else, or the error.
    using RunHandler = std::function<void(std::optional<std::string>&& result, GError*)>;
    void run(const std::string& body, const std::string& arguments, World, RunHandler&&);
    // Runs `body` like run(), then `then` once it answered or after `milliseconds`, whichever comes first.
    void runPatiently(const std::string& body, unsigned milliseconds, std::function<void()>&& then);
    static void answerEvaluation(const Reply&, const std::string& tagged);
    void wrappedMessage(const std::string& text);
    void bareMessage(const std::string& body, bool namesFrame);
    void documentReplaced();
    static void failEvaluations(std::map<uint64_t, Reply>&&, const char* message);

    // BlockPage.cpp
    void setDialogPolicy(const Json&, const Reply&);
    void answerDialog(const Json&, const Reply&);
    void setFiles(const Json&, const Reply&);
    void edit(const Json&, const Reply&);
    void cookies(const Json&, const Reply&);
    void setCookie(const Json&, const Reply&);
    void deleteCookie(const Json&, const Reply&);
    void query(const Json&, const Reply&);
    bool scriptDialog(WebKitScriptDialog*);
    static void finishDialog(WebKitScriptDialog*, bool accept, const std::optional<std::string>& text);
    void releaseHeldDialog(bool accept, const std::optional<std::string>& text);
    bool runFileChooser(WebKitFileChooserRequest*);
    static void answerChooser(WebKitFileChooserRequest*, const std::optional<std::vector<std::string>>& paths);
    using CookiesHandler = std::function<void(GList* cookies, GError*)>;
    void allCookies(CookiesHandler&&);

    // BlockInput.cpp
    void input(const Json&, const Reply&);
    void sendInput(const InputStep&);

    // BlockCapture.cpp
    void capture(const Json&, const Reply&);
    void startNextCapture();
    void grabStill(const std::shared_ptr<CaptureRequest>&);
    // Returns whether the frame's buffer is kept (presented).
    bool frameRendered(WPEBuffer*);
    void finishCapture(const std::shared_ptr<CaptureRequest>&, std::optional<Still>&&, const char* error);
    static void cancelCaptureTimer(CaptureRequest&);
    // Ends the capture under way when its request failed meanwhile, so it holds up no other.
    void releaseFailedCapture();

    // BlockPresent.cpp
    struct Presentation {
        // The client the frames go to; none for a presentation into a window.
        std::shared_ptr<PresentClient> client;
#if defined(_WIN32)
        // The client's window the block is presented into (`{"hwnd":N}`). Its web process presents there itself; a
        // frame that comes as a buffer is shown there by the host, which holds that buffer (`latest`) until the next.
        std::unique_ptr<PresentWindow> window;
#endif
        // The view's buffers the client was given, by their ids (per block, counted from 1).
        std::map<uint32_t, GRef<WPEBuffer>> buffers;
        std::map<WPEBuffer*, uint32_t> identifiers;
        // Buffers the client was sent a frame of and has not released.
        std::map<uint32_t, GRef<WPEBuffer>> shown;
        // The buffer of the last frame: what the client shows, which a capture starts from.
        GRef<WPEBuffer> latest;
        uint32_t lastBuffer { 0 };
    };
    void present(const Json&, const Reply&);
#if defined(_WIN32)
    void presentIntoWindow(const Json& window, const Reply&);
#endif
    // Ends any presentation and starts this one.
    void startPresenting(std::unique_ptr<Presentation>&&);
    // Ends the presentation, saying so to the client when `tellClient`.
    void stopPresenting(bool tellClient);
    bool presentFrame(WPEBuffer*);
    void presentedBuffersChanged(WPEBuffer** buffers, unsigned count);
    void cursorChanged(const char* name);
    // Whether frames are to be rendered: while presented or capturing. On Windows also whether the view presents
    // into the client's window: while presented into one and not capturing.
    void updateCompositionSuppressed();

    Host& m_host;
    uint64_t m_identifier;
    unsigned m_width;
    unsigned m_height;
    double m_scale { 1 };
    bool m_closed { false };
    // Whether the view holds the keyboard (`focus`).
    bool m_focused { true };

    GRef<WebKitNetworkSession> m_session;
    GRef<WebKitSettings> m_settings;
    GRef<WebKitUserContentManager> m_contentManager;
    GRef<WebKitWebView> m_view;
    WPEView* m_wpeView { nullptr };
    // The block of this page's Web Inspector, while it is open.
    std::weak_ptr<Block> m_inspector;
    // A Web Inspector's block (openInspector()): a view and no web view of its own.
    bool m_isInspector { false };
#if defined(_WIN32)
    // Whether the page draws with the GPU: unless the host gave it up (Host::drawsWithGPU()) before the page opened
    // or after its web process crashed.
    bool m_drawsWithGPU { true };
#endif

    // Events: the last `ringSize` of them, numbered from 1.
    static constexpr size_t ringSize = 4096;
    std::deque<std::string> m_events;
    uint64_t m_nextSequence { 1 };
    // An `events` call with `wait`, answered by the next event after `after`.
    Reply m_eventsWaiter;
    uint64_t m_eventsWaiterAfter { 0 };
    std::optional<std::pair<bool, bool>> m_reportedHistory;

    // Loads.
    bool m_loadFailed { false };
    bool m_errorPageShown { false };
    std::optional<std::string> m_errorPage;
    std::optional<std::string> m_error;

    // Settings.
    bool m_insecure { false };
    // The hosts whose certificate the current navigation accepted (loadFailedWithTLSErrors()).
    std::set<std::string> m_tlsAccepted;
    std::optional<std::pair<std::string, std::string>> m_credentials;
    std::optional<std::string> m_downloadDirectory;
    std::optional<std::vector<std::string>> m_allowedHosts;
    WebKitUserContentFilter* m_allowFilter { nullptr };
    uint64_t m_allowGeneration { 0 };
    std::vector<WebKitUserScript*> m_scripts;
    // Whether the page's own world has the two message handlers: only while a script of the list runs there.
    bool m_pageWorldHandlers { false };
    // The screen and window `identity` gave; without them the block's are made for its viewport.
    std::optional<ScreenProfile> m_pinnedScreen;
    std::optional<WindowProfile> m_pinnedWindow;

    // Resource loads under way.
    std::vector<Resource> m_resources;
    uint64_t m_lastResource { 0 };

    // Downloads.
    std::vector<Download> m_downloads;
    uint64_t m_lastDownload { 0 };

    // Dialogs and file choosers.
    DialogPolicy m_dialogPolicy { DialogPolicy::Default };
    WebKitScriptDialog* m_heldDialog { nullptr };
    GRef<WebKitFileChooserRequest> m_heldChooser;
    std::optional<std::vector<std::string>> m_presetFiles;

    // Frames by the names their scripts gave them: the index path from the main frame, when known.
    std::map<std::string, std::optional<std::vector<unsigned>>> m_frames;
    std::map<uint64_t, Reply> m_frameCalls;
    uint64_t m_lastFrameCall { 0 };
    // Evaluations in the main frame that have not answered yet.
    std::map<uint64_t, Reply> m_evaluations;
    uint64_t m_lastEvaluation { 0 };

    // Responsiveness: the calls into the page that have not answered (the operation, its answer and whether it
    // fails once the page is unresponsive), the round trip in flight (when it was sent, monotonic µs), and the
    // navigation the page has not taken up yet (it starts the navigation again).
    struct PageCall {
        std::string operation;
        std::weak_ptr<Responder> reply;
        bool failWhenUnresponsive { false };
    };
    std::vector<PageCall> m_pageCalls;
    std::optional<gint64> m_pingSent;
    uint64_t m_pingGeneration { 0 };
    guint m_pingTimer { 0 };
    guint m_nextPing { 0 };
    bool m_unresponsive { false };
    std::function<void()> m_pendingNavigation;
    // The round trip a navigation came after: a later one's answer means the page took the navigation up.
    uint64_t m_pendingNavigationAfter { 0 };

    // Input.
    unsigned m_pressedButtons { 0 };
    std::optional<std::pair<double, double>> m_pointer;

    // Captures, one at a time.
    std::deque<std::shared_ptr<CaptureRequest>> m_captures;
    std::shared_ptr<CaptureRequest> m_capture;
    std::function<void(WPEBuffer*)> m_frameWaiter;

    // Presentation.
    std::unique_ptr<Presentation> m_presentation;
    std::string m_cursor;
};

} // namespace AgentHost
