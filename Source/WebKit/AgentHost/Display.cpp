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

#include "Display.h"

#include "Utilities.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace AgentHost {

// The screen's refresh: one thread of the display's own, at each tick of the client's display while those come
// (none for a second: they stopped), else on a 60 Hz grid, as the engine's own timer would. At each refresh it
// calls the engine's sync callbacks while the engine observes the screen, and runs on the main loop what waits
// for the refresh (presented frames, shown at the refresh as on a display). It waits for nothing while neither
// is the case.

struct RefreshState {
    using Clock = std::chrono::steady_clock;

    std::mutex lock;
    std::condition_variable wake;
    WPEScreenSyncObserver* observer { nullptr };
    bool observing { false };
    unsigned pendingTicks { 0 };
    Clock::time_point lastTick;
    // What runs on the main loop after the next refresh. Only the main loop touches the list itself.
    std::vector<std::function<void()>> waiters;
    bool waiting { false };
};

// How long after its last tick a client's display no longer paces the pages, and the timer's period.
static constexpr std::chrono::seconds tickPatience { 1 };
static constexpr std::chrono::microseconds timerInterval { 16667 };

static void runRefreshThread(RefreshState&);

static RefreshState& refreshState()
{
    static RefreshState* state = [] {
        auto* state = new RefreshState;
        std::thread(runRefreshThread, std::ref(*state)).detach();
        return state;
    }();
    return *state;
}

static void runRefreshWaiters()
{
    auto& state = refreshState();
    std::vector<std::function<void()>> waiters;
    {
        std::lock_guard<std::mutex> locker(state.lock);
        waiters = std::exchange(state.waiters, { });
        state.waiting = false;
    }
    for (auto& waiter : waiters)
        waiter();
}

static void runRefreshThread(RefreshState& state)
{
    using Clock = RefreshState::Clock;
    auto nextTimerTick = Clock::now();
    std::unique_lock<std::mutex> locker(state.lock);
    while (true) {
        auto ticking = [&] {
            return state.lastTick != Clock::time_point { } && Clock::now() - state.lastTick < tickPatience;
        };
        bool refreshed = false;
        if (state.pendingTicks) {
            state.pendingTicks = 0;
            refreshed = true;
        } else if (!state.observing && !state.waiting) {
            state.wake.wait(locker);
            nextTimerTick = Clock::now();
            continue;
        } else if (ticking())
            state.wake.wait_until(locker, state.lastTick + tickPatience);
        else {
            auto now = Clock::now();
            if (now < nextTimerTick) {
                state.wake.wait_until(locker, nextTimerTick);
                continue;
            }
            // On a fixed grid, as a display: the time each refresh took is not added to the period.
            do {
                nextTimerTick += timerInterval;
            } while (nextTimerTick <= now);
            refreshed = true;
        }
        if (!refreshed)
            continue;
        auto* observer = state.observing ? state.observer : nullptr;
        bool waiting = state.waiting;
        locker.unlock();
        if (observer)
            WPE_SCREEN_SYNC_OBSERVER_GET_CLASS(observer)->sync(observer);
        if (waiting)
            runOnMainLoop(runRefreshWaiters);
        locker.lock();
    }
}

#define AGENT_HOST_TYPE_SYNC_OBSERVER (agent_host_sync_observer_get_type())
G_DECLARE_FINAL_TYPE(AgentHostSyncObserver, agent_host_sync_observer, AGENT_HOST, SYNC_OBSERVER, WPEScreenSyncObserver)

struct _AgentHostSyncObserver {
    WPEScreenSyncObserver parent;
};

G_DEFINE_FINAL_TYPE(AgentHostSyncObserver, agent_host_sync_observer, WPE_TYPE_SCREEN_SYNC_OBSERVER)

static void agentHostSyncObserverStart(WPEScreenSyncObserver* observer)
{
    auto& state = refreshState();
    {
        std::lock_guard<std::mutex> locker(state.lock);
        state.observer = observer;
        state.observing = true;
    }
    state.wake.notify_all();
}

static void agentHostSyncObserverStop(WPEScreenSyncObserver*)
{
    auto& state = refreshState();
    std::lock_guard<std::mutex> locker(state.lock);
    state.observing = false;
}

static void agent_host_sync_observer_class_init(AgentHostSyncObserverClass* observerClass)
{
    WPE_SCREEN_SYNC_OBSERVER_CLASS(observerClass)->start = agentHostSyncObserverStart;
    WPE_SCREEN_SYNC_OBSERVER_CLASS(observerClass)->stop = agentHostSyncObserverStop;
}

static void agent_host_sync_observer_init(AgentHostSyncObserver*)
{
}

// The one screen: its size says nothing to pages (a block's `identity` sets what they see); it is there for
// its sync observer.

#define AGENT_HOST_TYPE_SCREEN (agent_host_screen_get_type())
G_DECLARE_FINAL_TYPE(AgentHostScreen, agent_host_screen, AGENT_HOST, SCREEN, WPEScreen)

struct _AgentHostScreen {
    WPEScreen parent;
    WPEScreenSyncObserver* observer;
};

G_DEFINE_FINAL_TYPE(AgentHostScreen, agent_host_screen, WPE_TYPE_SCREEN)

static WPEScreenSyncObserver* agentHostScreenGetSyncObserver(WPEScreen* screen)
{
    return AGENT_HOST_SCREEN(screen)->observer;
}

static void agentHostScreenFinalize(GObject* object)
{
    g_clear_object(&AGENT_HOST_SCREEN(object)->observer);
    G_OBJECT_CLASS(agent_host_screen_parent_class)->finalize(object);
}

static void agent_host_screen_class_init(AgentHostScreenClass* screenClass)
{
    G_OBJECT_CLASS(screenClass)->finalize = agentHostScreenFinalize;
    WPE_SCREEN_CLASS(screenClass)->get_sync_observer = agentHostScreenGetSyncObserver;
}

static void agent_host_screen_init(AgentHostScreen* screen)
{
    screen->observer = WPE_SCREEN_SYNC_OBSERVER(g_object_new(AGENT_HOST_TYPE_SYNC_OBSERVER, nullptr));
}

// A toplevel that is nothing but its size, its scale and its state: active from the start.

#define AGENT_HOST_TYPE_TOPLEVEL (agent_host_toplevel_get_type())
G_DECLARE_FINAL_TYPE(AgentHostToplevel, agent_host_toplevel, AGENT_HOST, TOPLEVEL, WPEToplevel)

struct _AgentHostToplevel {
    WPEToplevel parent;
};

G_DEFINE_FINAL_TYPE(AgentHostToplevel, agent_host_toplevel, WPE_TYPE_TOPLEVEL)

static void agentHostToplevelConstructed(GObject* object)
{
    G_OBJECT_CLASS(agent_host_toplevel_parent_class)->constructed(object);
    wpe_toplevel_state_changed(WPE_TOPLEVEL(object), WPE_TOPLEVEL_STATE_ACTIVE);
}

static gboolean agentHostToplevelResize(WPEToplevel* toplevel, int width, int height)
{
    wpe_toplevel_resized(toplevel, width, height);
    wpe_toplevel_foreach_view(toplevel, [](WPEToplevel* toplevel, WPEView* view, gpointer) -> gboolean {
        int width, height;
        wpe_toplevel_get_size(toplevel, &width, &height);
        wpe_view_resized(view, width, height);
        return FALSE;
    }, nullptr);
    return TRUE;
}

static void agent_host_toplevel_class_init(AgentHostToplevelClass* toplevelClass)
{
    G_OBJECT_CLASS(toplevelClass)->constructed = agentHostToplevelConstructed;
    WPE_TOPLEVEL_CLASS(toplevelClass)->resize = agentHostToplevelResize;
}

static void agent_host_toplevel_init(AgentHostToplevel*)
{
}

// A view whose frames are handed to a handler and then reported as shown.

#define AGENT_HOST_TYPE_VIEW (agent_host_view_get_type())
G_DECLARE_FINAL_TYPE(AgentHostView, agent_host_view, AGENT_HOST, VIEW, WPEView)

struct _AgentHostView {
    WPEView parent;
    FrameHandler* frameHandler;
    CursorHandler* cursorHandler;
};

G_DEFINE_FINAL_TYPE(AgentHostView, agent_host_view, WPE_TYPE_VIEW)

struct RenderedFrame {
    WPEView* view;
    WPEBuffer* buffer;
};

static gboolean agentHostViewRenderBuffer(WPEView* view, WPEBuffer* buffer, const WPERectangle*, guint, GError**)
{
    // The engine expects to hear about the frame after this call returned.
    auto* frame = new RenderedFrame { WPE_VIEW(g_object_ref(view)), WPE_BUFFER(g_object_ref(buffer)) };
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto* frame = static_cast<RenderedFrame*>(data);
        auto* view = AGENT_HOST_VIEW(frame->view);
        bool kept = view->frameHandler && *view->frameHandler && (*view->frameHandler)(frame->buffer);
        if (!kept) {
            wpe_view_buffer_rendered(frame->view, frame->buffer);
            wpe_view_buffer_released(frame->view, frame->buffer);
        }
        return G_SOURCE_REMOVE;
    }, frame, [](gpointer data) {
        auto* frame = static_cast<RenderedFrame*>(data);
        g_object_unref(frame->buffer);
        g_object_unref(frame->view);
        delete frame;
    });
    return TRUE;
}

static void agentHostViewSetCursorFromName(WPEView* view, const char* name)
{
    auto* self = AGENT_HOST_VIEW(view);
    if (self->cursorHandler && *self->cursorHandler)
        (*self->cursorHandler)(name ? name : "default");
}

static void agentHostViewFinalize(GObject* object)
{
    delete AGENT_HOST_VIEW(object)->frameHandler;
    delete AGENT_HOST_VIEW(object)->cursorHandler;
    G_OBJECT_CLASS(agent_host_view_parent_class)->finalize(object);
}

static void agent_host_view_class_init(AgentHostViewClass* viewClass)
{
    G_OBJECT_CLASS(viewClass)->finalize = agentHostViewFinalize;
    WPE_VIEW_CLASS(viewClass)->render_buffer = agentHostViewRenderBuffer;
    WPE_VIEW_CLASS(viewClass)->set_cursor_from_name = agentHostViewSetCursorFromName;
}

static void agent_host_view_init(AgentHostView* view)
{
    view->frameHandler = nullptr;
    view->cursorHandler = nullptr;
}

// The display: it makes views and has the one screen.

#define AGENT_HOST_TYPE_DISPLAY (agent_host_display_get_type())
G_DECLARE_FINAL_TYPE(AgentHostDisplay, agent_host_display, AGENT_HOST, DISPLAY, WPEDisplay)

struct _AgentHostDisplay {
    WPEDisplay parent;
    int newViewWidth;
    int newViewHeight;
    WPEScreen* screen;
};

G_DEFINE_FINAL_TYPE(AgentHostDisplay, agent_host_display, WPE_TYPE_DISPLAY)

static gboolean agentHostDisplayConnect(WPEDisplay*, GError**)
{
    return TRUE;
}

static WPEView* agentHostDisplayCreateView(WPEDisplay* display)
{
    auto* self = AGENT_HOST_DISPLAY(display);
    auto* view = WPE_VIEW(g_object_new(AGENT_HOST_TYPE_VIEW, "display", display, nullptr));
    auto* toplevel = WPE_TOPLEVEL(g_object_new(AGENT_HOST_TYPE_TOPLEVEL, "display", display, nullptr));
    wpe_view_set_toplevel(view, toplevel);
    wpe_toplevel_resized(toplevel, self->newViewWidth, self->newViewHeight);
    wpe_view_resized(view, self->newViewWidth, self->newViewHeight);
    g_object_unref(toplevel);
    // To its page the view is an ordinary window in front: on screen, and holding the keyboard.
    wpe_view_set_visible(view, TRUE);
    wpe_view_map(view);
    wpe_view_focus_in(view);
    return view;
}

static guint agentHostDisplayGetNScreens(WPEDisplay*)
{
    return 1;
}

static WPEScreen* agentHostDisplayGetScreen(WPEDisplay* display, guint index)
{
    return index ? nullptr : AGENT_HOST_DISPLAY(display)->screen;
}

static void agentHostDisplayFinalize(GObject* object)
{
    g_clear_object(&AGENT_HOST_DISPLAY(object)->screen);
    G_OBJECT_CLASS(agent_host_display_parent_class)->finalize(object);
}

static void agent_host_display_class_init(AgentHostDisplayClass* displayClass)
{
    G_OBJECT_CLASS(displayClass)->finalize = agentHostDisplayFinalize;
    WPE_DISPLAY_CLASS(displayClass)->connect = agentHostDisplayConnect;
    WPE_DISPLAY_CLASS(displayClass)->create_view = agentHostDisplayCreateView;
    WPE_DISPLAY_CLASS(displayClass)->get_n_screens = agentHostDisplayGetNScreens;
    WPE_DISPLAY_CLASS(displayClass)->get_screen = agentHostDisplayGetScreen;
}

static void agent_host_display_init(AgentHostDisplay* display)
{
    display->newViewWidth = 1280;
    display->newViewHeight = 800;
    display->screen = WPE_SCREEN(g_object_new(AGENT_HOST_TYPE_SCREEN, "id", 1, nullptr));
    wpe_screen_set_size(display->screen, 1920, 1080);
    wpe_screen_set_physical_size(display->screen, 1920, 1080);
    wpe_screen_set_scale(display->screen, 1);
    wpe_screen_set_refresh_rate(display->screen, 60000);
}

WPEDisplay* createDisplay()
{
    auto* display = WPE_DISPLAY(g_object_new(AGENT_HOST_TYPE_DISPLAY, nullptr));
    GError* error = nullptr;
    if (!wpe_display_connect(display, &error)) {
        g_warning("the display did not connect: %s", error ? error->message : "no reason given");
        g_clear_error(&error);
        g_object_unref(display);
        return nullptr;
    }
    wpe_display_set_primary(display);
    // A desktop: pages are told there is a mouse to hover with and no screen to touch.
    wpe_display_set_available_input_devices(display, static_cast<WPEAvailableInputDevices>(WPE_AVAILABLE_INPUT_DEVICE_MOUSE | WPE_AVAILABLE_INPUT_DEVICE_KEYBOARD));
#if defined(__APPLE__)
    // `system-ui` is the Mac's own interface font, under the name fontconfig knows it by.
    if (!wpe_settings_set_string(wpe_display_get_settings(display), WPE_SETTING_FONT_NAME, "System Font 13", WPE_SETTINGS_SOURCE_APPLICATION, &error)) {
        g_warning("the system font was not set: %s", error ? error->message : "no reason given");
        g_clear_error(&error);
    }
#endif
    return display;
}

void setNewViewSize(WPEDisplay* display, int width, int height)
{
    auto* self = AGENT_HOST_DISPLAY(display);
    self->newViewWidth = width;
    self->newViewHeight = height;
}

void setFrameHandler(WPEView* view, FrameHandler&& handler)
{
    auto* self = AGENT_HOST_VIEW(view);
    delete self->frameHandler;
    self->frameHandler = handler ? new FrameHandler(std::move(handler)) : nullptr;
}

void setCursorHandler(WPEView* view, CursorHandler&& handler)
{
    auto* self = AGENT_HOST_VIEW(view);
    delete self->cursorHandler;
    self->cursorHandler = handler ? new CursorHandler(std::move(handler)) : nullptr;
}

void resizeView(WPEView* view, int width, int height)
{
    if (auto* toplevel = wpe_view_get_toplevel(view))
        wpe_toplevel_resize(toplevel, width, height);
    else
        wpe_view_resized(view, width, height);
}

void setViewScale(WPEView* view, double scale)
{
    if (auto* toplevel = wpe_view_get_toplevel(view))
        wpe_toplevel_scale_changed(toplevel, scale);
}

void displayTicked()
{
    auto& state = refreshState();
    {
        std::lock_guard<std::mutex> locker(state.lock);
        state.lastTick = RefreshState::Clock::now();
        state.pendingTicks++;
    }
    state.wake.notify_all();
}

void afterNextRefresh(std::function<void()>&& function)
{
    auto& state = refreshState();
    {
        std::lock_guard<std::mutex> locker(state.lock);
        state.waiters.push_back(std::move(function));
        state.waiting = true;
    }
    state.wake.notify_all();
}

} // namespace AgentHost
