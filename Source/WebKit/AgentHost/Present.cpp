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

#include "Present.h"

#include "Utilities.h"

#if defined(__APPLE__)
#include <IOSurface/IOSurfaceRef.h>
#include <atomic>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <thread>
#include <unistd.h>
#endif

namespace AgentHost {

#if defined(__APPLE__)

// The messages (docs/presentation.md, "The Mach channel"): `msgh_id` is the kind, the payload consecutive native-endian
// 32-bit words after the header, or after the body and its one port descriptor.
enum : mach_msg_id_t {
    helloMessage = 1,
    bufferMessage = 2,
    frameMessage = 3,
    goneMessage = 4,
    byeMessage = 5,
    releasedMessage = 16,
    tickMessage = 17,
};

static constexpr uint32_t channelVersion = 1;
// How long a message waits for room in the client's queue before the client counts as gone.
static constexpr mach_msg_timeout_t sendPatience = 1000;

template<size_t words> struct SimpleMessage {
    mach_msg_header_t header;
    uint32_t payload[words];
};

template<size_t words> struct PortMessage {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t payload[words];
};

// What the host receives: the client's messages, and the dead-name notification of the client's port.
struct ReceivedMessage {
    union {
        mach_msg_header_t header;
        SimpleMessage<2> released;
        mach_dead_name_notification_t deadName;
    };
    mach_msg_trailer_t trailer;
    uint8_t spare[64];
};

class MachClient final : public PresentClient, public std::enable_shared_from_this<MachClient> {
public:
    MachClient(std::string&& service, Handlers&& handlers)
        : m_service(std::move(service))
        , m_handlers(std::move(handlers))
    {
    }

    ~MachClient()
    {
        // Destroying the receive right ends the receiving thread's wait.
        if (m_receivePort != MACH_PORT_NULL)
            mach_port_mod_refs(mach_task_self(), m_receivePort, MACH_PORT_RIGHT_RECEIVE, -1);
        if (m_thread.joinable())
            m_thread.join();
        if (m_sendPort != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), m_sendPort);
    }

    bool connect(std::string& error)
    {
        if (bootstrap_look_up(bootstrap_port, m_service.c_str(), &m_sendPort) != KERN_SUCCESS) {
            m_sendPort = MACH_PORT_NULL;
            error = "no Mach service " + m_service;
            return false;
        }
        auto task = mach_task_self();
        if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &m_receivePort) != KERN_SUCCESS) {
            m_receivePort = MACH_PORT_NULL;
            error = "no Mach port for the client's messages";
            return false;
        }
        // Room for the ticks and releases between two turns of the thread.
        mach_port_limits_t limits { MACH_PORT_QLIMIT_LARGE };
        mach_port_set_attributes(task, m_receivePort, MACH_PORT_LIMITS_INFO, reinterpret_cast<mach_port_info_t>(&limits), MACH_PORT_LIMITS_INFO_COUNT);
        // A client that dies says so on the port.
        mach_port_t previous = MACH_PORT_NULL;
        if (mach_port_request_notification(task, m_sendPort, MACH_NOTIFY_DEAD_NAME, 0, m_receivePort, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous) != KERN_SUCCESS) {
            error = "the Mach service " + m_service + " is gone";
            return false;
        }

        PortMessage<2> hello { };
        hello.port.name = m_receivePort;
        hello.port.disposition = MACH_MSG_TYPE_MAKE_SEND;
        hello.port.type = MACH_MSG_PORT_DESCRIPTOR;
        hello.payload[0] = channelVersion;
        hello.payload[1] = static_cast<uint32_t>(getpid());
        if (!send(hello, helloMessage)) {
            error = "the Mach service " + m_service + " took no message";
            return false;
        }

        m_thread = std::thread([weakThis = weak_from_this(), port = m_receivePort, ticked = m_handlers.ticked] {
            receive(weakThis, port, ticked);
        });
        return true;
    }

    const std::string& service() const override { return m_service; }
    bool alive() const override { return m_alive; }

    bool sendBuffer(uint32_t block, uint32_t buffer, WPEBuffer* wpeBuffer) override
    {
        if (!WPE_IS_BUFFER_IOSURFACE(wpeBuffer))
            return true;
        auto* surface = wpe_buffer_iosurface_get_surface(WPE_BUFFER_IOSURFACE(wpeBuffer));
        PortMessage<4> message { };
        message.port.name = IOSurfaceCreateMachPort(surface);
        message.port.disposition = MACH_MSG_TYPE_MOVE_SEND;
        message.port.type = MACH_MSG_PORT_DESCRIPTOR;
        message.payload[0] = block;
        message.payload[1] = buffer;
        message.payload[2] = static_cast<uint32_t>(IOSurfaceGetWidth(surface));
        message.payload[3] = static_cast<uint32_t>(IOSurfaceGetHeight(surface));
        if (message.port.name == MACH_PORT_NULL)
            return true;
        return send(message, bufferMessage);
    }

    bool sendFrame(uint32_t block, uint32_t buffer) override
    {
        SimpleMessage<2> message { };
        message.payload[0] = block;
        message.payload[1] = buffer;
        return send(message, frameMessage);
    }

    bool sendGone(uint32_t block, uint32_t buffer) override
    {
        SimpleMessage<2> message { };
        message.payload[0] = block;
        message.payload[1] = buffer;
        return send(message, goneMessage);
    }

    bool sendBye(uint32_t block) override
    {
        SimpleMessage<1> message { };
        message.payload[0] = block;
        return send(message, byeMessage);
    }

private:
    template<size_t words> static constexpr bool isComplex(const SimpleMessage<words>&) { return false; }
    template<size_t words> static constexpr bool isComplex(const PortMessage<words>&) { return true; }

    template<typename Message> bool send(Message& message, mach_msg_id_t identifier)
    {
        if (!m_alive)
            return false;
        message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | (isComplex(message) ? MACH_MSGH_BITS_COMPLEX : 0);
        message.header.msgh_size = sizeof(message);
        message.header.msgh_remote_port = m_sendPort;
        message.header.msgh_local_port = MACH_PORT_NULL;
        message.header.msgh_id = identifier;
        if constexpr (requires { message.body; })
            message.body.msgh_descriptor_count = 1;
        auto result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0, MACH_PORT_NULL, sendPatience, MACH_PORT_NULL);
        if (result == MACH_MSG_SUCCESS)
            return true;
        // A right that was not sent is still the host's.
        if constexpr (requires { message.port; }) {
            if (message.port.disposition == MACH_MSG_TYPE_MOVE_SEND && message.port.name != MACH_PORT_NULL)
                mach_port_deallocate(mach_task_self(), message.port.name);
        }
        g_warning("the Mach service %s took no message: %s", m_service.c_str(), mach_error_string(result));
        lost();
        return false;
    }

    void lost()
    {
        if (!m_alive.exchange(false))
            return;
        runOnMainLoop([weakThis = weak_from_this()] {
            if (auto client = weakThis.lock()) {
                if (auto gone = std::exchange(client->m_handlers.gone, nullptr))
                    gone();
            }
        });
    }

    // The receiving thread. It touches nothing of the client but through the main loop, but the ticks, which
    // go straight to the screen.
    static void receive(std::weak_ptr<MachClient> weakThis, mach_port_t port, std::function<void()> ticked)
    {
        while (true) {
            ReceivedMessage message { };
            auto result = mach_msg(&message.header, MACH_RCV_MSG, 0, sizeof(message), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            if (result == MACH_RCV_TOO_LARGE)
                continue;
            if (result != MACH_MSG_SUCCESS)
                return;
            switch (message.header.msgh_id) {
            case tickMessage:
                if (ticked)
                    ticked();
                break;
            case releasedMessage:
                if (message.header.msgh_size >= sizeof(SimpleMessage<2>)) {
                    uint32_t block = message.released.payload[0];
                    uint32_t buffer = message.released.payload[1];
                    runOnMainLoop([weakThis, block, buffer] {
                        auto client = weakThis.lock();
                        if (client && client->m_alive && client->m_handlers.released)
                            client->m_handlers.released(block, buffer);
                    });
                }
                break;
            case MACH_NOTIFY_DEAD_NAME:
                mach_port_deallocate(mach_task_self(), message.deadName.not_port);
                runOnMainLoop([weakThis] {
                    if (auto client = weakThis.lock())
                        client->lost();
                });
                break;
            default:
                mach_msg_destroy(&message.header);
                break;
            }
        }
    }

    std::string m_service;
    Handlers m_handlers;
    mach_port_t m_sendPort { MACH_PORT_NULL };
    mach_port_t m_receivePort { MACH_PORT_NULL };
    std::atomic<bool> m_alive { true };
    std::thread m_thread;
};

std::shared_ptr<PresentClient> PresentClient::connect(const std::string& service, Handlers&& handlers, std::string& error)
{
    auto client = std::make_shared<MachClient>(std::string(service), std::move(handlers));
    if (!client->connect(error))
        return nullptr;
    return client;
}

#else

std::shared_ptr<PresentClient> PresentClient::connect(const std::string&, Handlers&&, std::string& error)
{
    error = "present: this system has no presentation yet";
    return nullptr;
}

#endif

} // namespace AgentHost
