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

// The protocol stream: frames of a 4-byte little-endian length and that many
// bytes of UTF-8 JSON, read from one descriptor and written to another.
// Reading and writing happen on threads of their own, so that neither a slow
// peer nor a large frame holds up the main loop; frames are handed over and
// taken on the main loop.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace AgentHost {

class Transport {
public:
    // The largest payload a frame may carry.
    static constexpr size_t maxFrameSize = 256u << 20;

    // Called on the main loop with each frame's payload.
    using FrameHandler = std::function<void(std::string&&)>;
    // Called on the main loop once, when the peer is gone: the input ended
    // (no error), or reading or writing failed (what went wrong).
    using ClosedHandler = std::function<void(const char* error)>;

    Transport(int input, int output, FrameHandler&&, ClosedHandler&&);
    ~Transport();

    void start();

    // Queues a frame. Main loop only.
    void send(std::string_view payload);

    // Writes what is queued and stops writing. Frames sent later are dropped.
    void finish();

private:
    void read();
    void write();
    void closed(const char* error);

    int m_input;
    int m_output;
    FrameHandler m_frameHandler;
    ClosedHandler m_closedHandler;

    std::thread m_reader;
    std::thread m_writer;
    std::mutex m_lock;
    std::condition_variable m_condition;
    std::deque<std::string> m_queue;
    bool m_finishing { false };
    bool m_started { false };
};

} // namespace AgentHost
