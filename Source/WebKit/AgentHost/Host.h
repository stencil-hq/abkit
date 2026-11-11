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

// The protocol's process side: the handshake, the envelope, the operations
// that belong to no block (`open`, `blocks`, `clipboard`) and what blocks
// share: the display, the website data stores and the frame stream.

#include "Json.h"
#include "Present.h"
#include "Transport.h"
#include "Utilities.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

namespace AgentHost {

class Block;

// The answer one request is owed. It is given once: by whoever handles the
// request, or with a failure when the last holder lets go of it unanswered
// (the block closed, the page went away).
class Responder {
public:
    Responder(std::weak_ptr<Transport>, const char* channel, std::string&& id);
    ~Responder();

    void ok(std::string_view value = "{}");
    void ok(const Json& value) { ok(value.serialize()); }
    void fail(const char* kind, std::string_view message);
    void invalid(std::string_view message) { fail("invalid", message); }
    void notFound(std::string_view message) { fail("not_found", message); }
    void unsupported(std::string_view message) { fail("unsupported", message); }
    void failed(std::string_view message) { fail("failed", message); }

    // What is said when the request is dropped without an answer.
    void setUnansweredMessage(std::string&& message) { m_unanswered = std::move(message); }

private:
    void send(std::string_view body);

    std::weak_ptr<Transport> m_transport;
    const char* m_channel;
    std::string m_id;
    std::string m_unanswered { "the browser block closed before answering" };
    bool m_answered { false };
};

using Reply = std::shared_ptr<Responder>;

// The `scale` member of `open` and `viewport` (device pixels per CSS pixel, 0.25 to 8), left as it is when
// missing. Returns false after refusing the request.
bool readScale(const Json& operation, double& scale, const Reply&);

class Host {
public:
    // `dataDirectory`: where profiles keep their website data; empty for none (every profile is then ephemeral).
    // `quit` is called once, with the process's exit status, when the host is done.
    Host(std::string&& dataDirectory, std::function<void(int)>&& quit);
    ~Host();

    // Starts serving the protocol on the two descriptors. Returns false when the host cannot run.
    bool start(int input, int output);
    // Closes every block and writes out what is still queued.
    void stop();

    WPEDisplay* display() const { return m_display.get(); }
    // The context of every block's web view: it loads the host's extension into their web processes.
    WebKitWebContext* context() const { return m_context.get(); }
    // The token tagging this process's calls into frames.
    const std::string& frameToken() const { return m_frameToken; }
    // A directory for what is not to outlive this process: made when first asked for, removed by stop().
    // Empty when it cannot be made.
    const std::string& temporaryDirectory();
    // Dark or light appearance. It is the display's, so every block's: the last request wins.
    void setDarkAppearance(bool);

    // The client a `present` target names (`{"mach":NAME}` on macOS), reached once and shared by every block
    // presented to it. Null, after failing `reply`, when it cannot be reached.
    std::shared_ptr<PresentClient> presentClient(const Json& target, const Reply&);

private:
    void handleFrame(std::string&&);
    void handleClosed(const char* error);
    void dispatch(const Json& operation, const Reply&);

    void open(const Json& operation, const Reply&);
    void listBlocks(const Reply&);
    void clipboard(const Json& operation, const Reply&);
    void clearProfile(const Json& operation, const Reply&);
    void closeBlock(uint64_t, const Reply&);
    void presentClientGone(const std::string& service);

    GRef<WebKitNetworkSession> sessionForProfile(const std::optional<std::string>& profile);
    GRef<WebKitNetworkSession> sessionInDirectory(const std::string& directory);

    std::string m_dataDirectory;
    std::string m_temporaryDirectory;
    uint64_t m_lastPrivateStore { 0 };
    std::function<void(int)> m_quit;
    std::shared_ptr<Transport> m_transport;
    GRef<WPEDisplay> m_display;
    GRef<WebKitWebContext> m_context;
    std::map<std::string, GRef<WebKitNetworkSession>> m_sessions;
    std::map<uint64_t, std::shared_ptr<Block>> m_blocks;
    // The clients blocks are presented to, by service.
    std::map<std::string, std::shared_ptr<PresentClient>> m_presentClients;
    uint64_t m_lastBlock { 0 };
    std::string m_frameToken;
};

} // namespace AgentHost
