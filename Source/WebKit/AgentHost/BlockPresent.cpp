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

#include <cmath>
#include <set>

namespace AgentHost {

// Frames shown by a client. A presented block renders like a page on
// screen, and each frame's buffer goes to the client as it is (an IOSurface
// on macOS): the client is given each buffer once, then the frames drawn
// into it, and hands each back when it no longer shows it; only then is the
// buffer drawn into again. The engine says which buffers exist (it makes
// new ones at a new size), and the client is told of each it no longer has.
//
// On Windows a block is presented into a window of the client's instead: its
// web process presents into that window itself, and nothing goes to the
// client. Frames that still come as buffers are shown in it by the host.

void Block::present(const Json& operation, const Reply& reply)
{
    auto to = operation.get("to");
    if (!to) {
        reply->invalid("present: `to` is a target or null");
        return;
    }
    if (to.isNull()) {
        stopPresenting(true);
        reply->ok();
        return;
    }
#if defined(_WIN32)
    if (to.isObject()) {
        if (auto window = to.get("hwnd")) {
            presentIntoWindow(window, reply);
            return;
        }
    }
#endif
    if (m_identifier > UINT32_MAX) {
        reply->failed("present: the block's id does not fit the channel's 32 bits");
        return;
    }
    auto client = m_host.presentClient(to, reply);
    if (!client)
        return;
    if (m_presentation && m_presentation->client == client) {
        reply->ok();
        return;
    }
    auto presentation = std::make_unique<Presentation>();
    presentation->client = std::move(client);
    startPresenting(std::move(presentation));
    reply->ok();
}

#if defined(_WIN32)
void Block::presentIntoWindow(const Json& target, const Reply& reply)
{
    // An HWND, as a number (sign-extended or not: only its low 32 bits mean something).
    double number = target.isNumber() ? target.toNumber() : 0;
    if (!number || number != std::floor(number) || std::abs(number) > 9007199254740992.0) {
        reply->invalid("present: `hwnd` is a window's handle, a number");
        return;
    }
    auto* window = reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<int64_t>(number)));
    if (!PresentWindow::isWindow(window)) {
        reply->invalid("present: `hwnd` is no window");
        return;
    }
    if (m_presentation && m_presentation->window && m_presentation->window->window() == window) {
        reply->ok();
        return;
    }
    auto presentation = std::make_unique<Presentation>();
    presentation->window = std::make_unique<PresentWindow>(window);
    // updateCompositionSuppressed() gives the view the window.
    startPresenting(std::move(presentation));
    reply->ok();
}
#endif

void Block::startPresenting(std::unique_ptr<Presentation>&& presentation)
{
    stopPresenting(true);
    m_presentation = std::move(presentation);
    // Rendering again, the view gives its next frame at once.
    updateCompositionSuppressed();
    if (!m_cursor.empty()) {
        auto event = eventOfType("cursor");
        event.setString("name", m_cursor);
        record(std::move(event));
    }
}

void Block::stopPresenting(bool tellClient)
{
    auto presentation = std::exchange(m_presentation, nullptr);
    if (!presentation)
        return;
    // What the client still showed is the view's again.
    for (auto& [identifier, buffer] : presentation->shown) {
        if (m_wpeView)
            wpe_view_buffer_released(m_wpeView, buffer.get());
    }
#if defined(_WIN32)
    // The window keeps the last frame the web process presented into it; the host's own window in it goes (with
    // `presentation`), and the buffer it showed is the view's again.
    if (presentation->window && m_wpeView) {
        wpe_view_set_native_window(m_wpeView, nullptr);
        if (presentation->latest)
            wpe_view_buffer_released(m_wpeView, presentation->latest.get());
    }
#endif
    if (tellClient && presentation->client && presentation->client->alive())
        presentation->client->sendBye(static_cast<uint32_t>(m_identifier));
    if (!m_closed)
        updateCompositionSuppressed();
}

bool Block::presentFrame(WPEBuffer* buffer)
{
    auto& presentation = *m_presentation;
#if defined(_WIN32)
    if (presentation.window) {
        // The web process does not present into the window: it cannot, and the host shows the frame there, holding
        // its buffer until the next one. Or a capture took the window from it meanwhile: the frame is not shown.
        if (wpe_view_get_native_window(m_wpeView) != presentation.window->window())
            return false;
        if (!presentation.window->show(buffer)) {
            // The client destroyed its window: nothing shows the frame held either.
            if (auto held = std::exchange(presentation.latest, { })) {
                presentation.window->forget(held.get());
                wpe_view_buffer_released(m_wpeView, held.get());
            }
            return false;
        }
        if (presentation.latest && presentation.latest.get() != buffer)
            wpe_view_buffer_released(m_wpeView, presentation.latest.get());
        presentation.latest = GRef<WPEBuffer>::retain(buffer);
        afterNextRefresh([view = GRef<WPEView>::retain(m_wpeView), buffer = GRef<WPEBuffer>::retain(buffer)] {
            wpe_view_buffer_rendered(view.get(), buffer.get());
        });
        return true;
    }
#endif
    auto& client = *presentation.client;
#if defined(__APPLE__)
    if (!WPE_IS_BUFFER_IOSURFACE(buffer)) {
#else
    // Only IOSurfaces are presented, and other systems have none (nor a presentation to start).
    {
#endif
        g_warning("block %" G_GUINT64_FORMAT ": a frame in %s cannot be presented without copies, and is not", m_identifier, G_OBJECT_TYPE_NAME(buffer));
        return false;
    }
    auto block = static_cast<uint32_t>(m_identifier);
    uint32_t identifier;
    if (auto found = presentation.identifiers.find(buffer); found != presentation.identifiers.end())
        identifier = found->second;
    else {
        identifier = ++presentation.lastBuffer;
        if (!client.sendBuffer(block, identifier, buffer))
            return false;
        presentation.identifiers.emplace(buffer, identifier);
        presentation.buffers.emplace(identifier, GRef<WPEBuffer>::retain(buffer));
    }
    if (!client.sendFrame(block, identifier))
        return false;
    presentation.shown[identifier] = GRef<WPEBuffer>::retain(buffer);
    presentation.latest = GRef<WPEBuffer>::retain(buffer);
    // On a display a frame is shown at the refresh after it came: the page's next frame is paced by that.
    afterNextRefresh([view = GRef<WPEView>::retain(m_wpeView), buffer = GRef<WPEBuffer>::retain(buffer)] {
        wpe_view_buffer_rendered(view.get(), buffer.get());
    });
    return true;
}

void Block::presentedBufferReleased(uint32_t buffer)
{
    if (!m_presentation || m_closed)
        return;
    auto found = m_presentation->shown.find(buffer);
    if (found == m_presentation->shown.end())
        return;
    auto released = std::move(found->second);
    m_presentation->shown.erase(found);
    wpe_view_buffer_released(m_wpeView, released.get());
}

void Block::presentedBuffersChanged(WPEBuffer** buffers, unsigned count)
{
    if (!m_presentation)
        return;
    std::set<WPEBuffer*> current(buffers, buffers + count);
    auto& presentation = *m_presentation;
#if defined(_WIN32)
    if (presentation.window) {
        if (presentation.latest && !current.count(presentation.latest.get())) {
            presentation.window->forget(presentation.latest.get());
            presentation.latest.clear();
        }
        return;
    }
#endif
    for (auto it = presentation.buffers.begin(); it != presentation.buffers.end();) {
        auto* buffer = it->second.get();
        if (current.count(buffer)) {
            ++it;
            continue;
        }
        auto identifier = it->first;
        presentation.shown.erase(identifier);
        if (presentation.latest.get() == buffer)
            presentation.latest.clear();
        presentation.identifiers.erase(buffer);
        it = presentation.buffers.erase(it);
        if (!presentation.client->sendGone(static_cast<uint32_t>(m_identifier), identifier))
            return;
    }
}

void Block::presentClientGone(const PresentClient& client)
{
    if (m_presentation && m_presentation->client.get() == &client)
        stopPresenting(false);
}

void Block::cursorChanged(const char* name)
{
    if (m_cursor == name)
        return;
    m_cursor = name;
    // Only a client showing the page sees its cursor.
    if (!m_presentation)
        return;
    auto event = eventOfType("cursor");
    event.setString("name", m_cursor);
    record(std::move(event));
}

void Block::updateCompositionSuppressed()
{
    setFeature("CompositionSuppressed", !m_presentation && !m_capture);
#if defined(_WIN32)
    // A capture takes its frames as buffers: meanwhile the web process presents nothing into the window, which
    // keeps the last frame.
    if (m_wpeView)
        wpe_view_set_native_window(m_wpeView, m_presentation && m_presentation->window && !m_capture ? m_presentation->window->window() : nullptr);
#endif
}

} // namespace AgentHost
