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
// messages, taking back its releases and its display's ticks (Protocol.md,
// "The Mach channel"). Other systems have no presentation yet.

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

} // namespace AgentHost
