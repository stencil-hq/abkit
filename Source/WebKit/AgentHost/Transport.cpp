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

#include "Transport.h"

#include "Platform.h"
#include "Utilities.h"
#include <cstdint>

namespace AgentHost {

Transport::Transport(int input, int output, FrameHandler&& frameHandler, ClosedHandler&& closedHandler)
    : m_input(input)
    , m_output(output)
    , m_frameHandler(std::move(frameHandler))
    , m_closedHandler(std::move(closedHandler))
{
}

Transport::~Transport()
{
    finish();
    // The reader may be blocked reading for as long as the peer lives.
    if (m_reader.joinable())
        m_reader.detach();
}

void Transport::start()
{
    m_started = true;
    m_reader = std::thread([this] { read(); });
    m_writer = std::thread([this] { write(); });
}

void Transport::send(std::string_view payload)
{
    if (payload.size() > maxFrameSize) {
        g_warning("dropping a %zu byte frame: frames carry at most %zu bytes", payload.size(), maxFrameSize);
        return;
    }
    std::string frame;
    frame.reserve(payload.size() + 4);
    auto size = static_cast<uint32_t>(payload.size());
    for (unsigned shift = 0; shift < 32; shift += 8)
        frame.push_back(static_cast<char>((size >> shift) & 0xff));
    frame.append(payload);
    {
        std::lock_guard locker(m_lock);
        if (m_finishing)
            return;
        m_queue.push_back(std::move(frame));
    }
    m_condition.notify_one();
}

void Transport::finish()
{
    {
        std::lock_guard locker(m_lock);
        m_finishing = true;
    }
    m_condition.notify_one();
    if (m_writer.joinable())
        m_writer.join();
}

void Transport::closed(const char* error)
{
    // The handler and what it captured belong to the main loop.
    runOnMainLoop([this, error] {
        if (auto handler = std::exchange(m_closedHandler, nullptr))
            handler(error);
    });
}

static bool readExactly(int descriptor, char* buffer, size_t size, bool& ended)
{
    size_t have = 0;
    while (have < size) {
        long count = Platform::readSome(descriptor, buffer + have, size - have);
        if (count <= 0) {
            ended = !count && !have;
            return false;
        }
        have += static_cast<size_t>(count);
    }
    return true;
}

void Transport::read()
{
    while (true) {
        unsigned char head[4];
        bool ended = false;
        if (!readExactly(m_input, reinterpret_cast<char*>(head), sizeof(head), ended)) {
            closed(ended ? nullptr : "the input ended inside a frame");
            return;
        }
        size_t size = head[0] | (head[1] << 8) | (head[2] << 16) | (static_cast<size_t>(head[3]) << 24);
        if (size > maxFrameSize) {
            closed("frame too large");
            return;
        }
        std::string payload(size, '\0');
        if (size && !readExactly(m_input, payload.data(), size, ended)) {
            closed("the input ended inside a frame");
            return;
        }
        runOnMainLoop([this, payload = std::move(payload)]() mutable {
            if (m_frameHandler)
                m_frameHandler(std::move(payload));
        });
    }
}

void Transport::write()
{
    while (true) {
        std::string frame;
        {
            std::unique_lock locker(m_lock);
            m_condition.wait(locker, [this] { return m_finishing || !m_queue.empty(); });
            if (m_queue.empty())
                return;
            frame = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!Platform::writeAll(m_output, frame.data(), frame.size())) {
            {
                std::lock_guard locker(m_lock);
                m_finishing = true;
                m_queue.clear();
            }
            closed("the output is closed");
            return;
        }
    }
}

} // namespace AgentHost
