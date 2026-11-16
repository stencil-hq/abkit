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

// Presenting blocks' frames to another process without copies. On macOS a
// client checks in a Mach service, and the host sends it the buffers
// (IOSurfaces) and frames of the blocks presented to it over plain Mach
// messages, taking back its releases and its display's ticks
// (docs/presentation.md, "The Mach channel"). On Windows a block is
// presented into a window of the client's instead (PresentWindow). Other
// systems have no presentation yet.

#include "Utilities.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <wpe/wpe-platform.h>

namespace AgentHost {

class PresentClient {
public:
    struct Handlers {
        // The client no longer shows buffer `buffer` of block `block`. On the main loop.
        std::function<void(uint32_t block, uint32_t buffer)> released;
        // The client's display refreshed. On a thread of the channel's.
        std::function<void()> ticked;
        // The client is gone (its service died, or a message could not reach it). On the main loop, once.
        std::function<void()> gone;
    };

    // Reaches the client's service, sending it `hello`. Null, and `error` set, when it cannot be reached or the
    // system has no presentation.
    static std::shared_ptr<PresentClient> connect(const std::string& service, Handlers&&, std::string& error);
    virtual ~PresentClient() = default;

    virtual const std::string& service() const = 0;
    virtual bool alive() const = 0;

    // Each returns false when the client could not be reached: it is then gone.
    // A buffer of a block: its surface and its size in device pixels. Only IOSurface buffers can be sent.
    virtual bool sendBuffer(uint32_t block, uint32_t buffer, WPEBuffer*) = 0;
    // A frame of a block is in that buffer.
    virtual bool sendFrame(uint32_t block, uint32_t buffer) = 0;
    // The host destroyed a buffer of a block.
    virtual bool sendGone(uint32_t block, uint32_t buffer) = 0;
    // The block is no longer presented.
    virtual bool sendBye(uint32_t block) = 0;
};

#if defined(_WIN32)
// A window of the client's a block is presented into (Windows, `{"hwnd":N}`). The block's web process presents
// its frames straight into it, with a swap chain of its own (wpe_view_set_native_window()); frames that still
// come as buffers (it cannot present into the window, e.g. without hardware acceleration) are shown here, in a
// window of the host's inside the client's: the size of the frame, at its top left, below the window the web
// process presents into, and like that one leaving all input to the client's window. The client may destroy its
// window at any time: then nothing is shown.
class PresentWindow {
public:
    // Whether `window` (an HWND) is a window.
    static bool isWindow(void* window);

    explicit PresentWindow(void* window);
    ~PresentWindow();
    PresentWindow(const PresentWindow&) = delete;
    PresentWindow& operator=(const PresentWindow&) = delete;

    // The client's window.
    void* window() const { return m_window; }
    // Shows a frame: the buffer's pixels, 32-bit BGRA. False when it cannot (the client's window is gone).
    bool show(WPEBuffer*);
    // The buffer shown is gone: nothing is painted from it again.
    void forget(WPEBuffer*);

    // For the host's window's procedure: paints the frame shown, and hears that the window is gone.
    void paint(void* dc);
    void childDestroyed() { m_child = nullptr; }

private:
    void* m_window;
    void* m_child { nullptr };
    GRef<WPEBuffer> m_shown;
};
#endif

} // namespace AgentHost
