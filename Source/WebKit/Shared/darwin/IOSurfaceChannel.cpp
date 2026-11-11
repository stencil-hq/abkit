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

#include "config.h"
#include "IOSurfaceChannel.h"

#if OS(DARWIN) && ENABLE(WPE_PLATFORM)

#include <bsm/libbsm.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <unistd.h>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <array>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace WebKit::IOSurfaceChannel {

struct SurfaceMessage {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t surface;
    uint64_t bufferID;
};

struct ReceivedSurfaceMessage {
    SurfaceMessage message;
    mach_msg_audit_trailer_t trailer;
};

// How long a send waits for room on the UI process's port, and a take for
// a surface that must already be queued, before giving up.
static constexpr mach_msg_timeout_t timeoutMilliseconds = 1000;

static std::array<char, sizeof(name_t)> serviceName(pid_t uiProcess)
{
    std::array<char, sizeof(name_t)> name { };
    auto string = makeString("org.webkit.wpe.iosurfaces."_s, uiProcess).latin1();
    memcpySpan(std::span { name }.first(string.length()), string.span());
    return name;
}

// UI process.

struct Received {
    pid_t process;
    uint64_t bufferID;
    mach_port_t surface;
};

static Lock receiverLock;
static mach_port_t receivePort WTF_GUARDED_BY_LOCK(receiverLock) = MACH_PORT_NULL;
static Vector<Received> received WTF_GUARDED_BY_LOCK(receiverLock);

bool listen()
{
    Locker locker { receiverLock };
    if (receivePort != MACH_PORT_NULL)
        return true;

    auto name = serviceName(getpid());
    mach_port_t port = MACH_PORT_NULL;
    if (bootstrap_check_in(bootstrap_port, name.data(), &port) != KERN_SUCCESS)
        return false;

    // Room for every surface of every web process between two turns of the
    // UI process's loop.
    mach_port_limits_t limits { MACH_PORT_QLIMIT_LARGE };
    mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, reinterpret_cast<mach_port_info_t>(&limits), MACH_PORT_LIMITS_INFO_COUNT);
    receivePort = port;
    return true;
}

// Moves the next message of the port to `received`, waiting up to `timeout`.
static bool receiveOne(mach_msg_timeout_t timeout) WTF_REQUIRES_LOCK(receiverLock)
{
    ReceivedSurfaceMessage message { };
    auto options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
    auto result = mach_msg(&message.message.header, options, 0, sizeof(message), receivePort, timeout, MACH_PORT_NULL);
    if (result != MACH_MSG_SUCCESS)
        return false;

    if (!(message.message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) || message.message.body.msgh_descriptor_count != 1
        || message.message.header.msgh_size != sizeof(SurfaceMessage) || message.message.surface.type != MACH_MSG_PORT_DESCRIPTOR) {
        mach_msg_destroy(&message.message.header);
        return true;
    }

    received.append({ audit_token_to_pid(message.trailer.msgh_audit), message.message.bufferID, message.message.surface.name });
    return true;
}

static size_t findReceived(pid_t process, uint64_t bufferID) WTF_REQUIRES_LOCK(receiverLock)
{
    return received.findIf([&](auto& item) {
        return item.process == process && item.bufferID == bufferID;
    });
}

IOSurfaceRef takeSurface(pid_t process, uint64_t bufferID)
{
    Locker locker { receiverLock };
    if (receivePort == MACH_PORT_NULL)
        return nullptr;

    // Everything already queued, then (it should not come to that) whatever
    // arrives next, until the surface turns up.
    while (receiveOne(0)) { }
    auto index = findReceived(process, bufferID);
    while (index == notFound && receiveOne(timeoutMilliseconds))
        index = findReceived(process, bufferID);
    if (index == notFound)
        return nullptr;

    auto port = received[index].surface;
    received.removeAt(index);
    auto surface = IOSurfaceLookupFromMachPort(port);
    mach_port_deallocate(mach_task_self(), port);
    return surface;
}

// Web process.

static mach_port_t sendPort()
{
    static mach_port_t port = MACH_PORT_NULL;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        auto name = serviceName(getppid());
        if (bootstrap_look_up(bootstrap_port, name.data(), &port) != KERN_SUCCESS)
            port = MACH_PORT_NULL;
    });
    return port;
}

bool isAvailable()
{
    return sendPort() != MACH_PORT_NULL;
}

bool sendSurface(IOSurfaceRef surface, uint64_t bufferID)
{
    auto port = sendPort();
    if (port == MACH_PORT_NULL)
        return false;

    SurfaceMessage message { };
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(message);
    message.header.msgh_remote_port = port;
    message.body.msgh_descriptor_count = 1;
    message.surface.name = IOSurfaceCreateMachPort(surface);
    message.surface.disposition = MACH_MSG_TYPE_MOVE_SEND;
    message.surface.type = MACH_MSG_PORT_DESCRIPTOR;
    message.bufferID = bufferID;
    if (message.surface.name == MACH_PORT_NULL)
        return false;

    auto result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0, MACH_PORT_NULL, timeoutMilliseconds, MACH_PORT_NULL);
    if (result != MACH_MSG_SUCCESS) {
        // The send right was not consumed.
        mach_port_deallocate(mach_task_self(), message.surface.name);
        return false;
    }
    return true;
}

} // namespace WebKit::IOSurfaceChannel

#endif // OS(DARWIN) && ENABLE(WPE_PLATFORM)
