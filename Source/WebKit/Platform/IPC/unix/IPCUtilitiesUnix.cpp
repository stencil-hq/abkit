/*
 * Copyright (C) 2011, 2025 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "IPCUtilities.h"

#if USE(UNIX_DOMAIN_SOCKETS)
#include <sys/socket.h>
#include <wtf/UniStdExtras.h>

namespace IPC {

#if OS(DARWIN)
// Darwin's Unix sockets come as streams and datagrams only. A datagram is at
// most the sender's send buffer (2048 bytes unless raised) and is refused
// (ENOBUFS) while the receiver's buffer has no room for it.
static constexpr int darwinSendBufferSize = 256 * 1024;
static constexpr int darwinReceiveBufferSize = 4 * 1024 * 1024;

static void sizeDatagramSocket(int socket)
{
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &darwinSendBufferSize, sizeof(darwinSendBufferSize));
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &darwinReceiveBufferSize, sizeof(darwinReceiveBufferSize));
}
#endif

SocketPair createPlatformConnection(int socketType, unsigned options)
{
    std::array<int, 2> sockets;

#if OS(DARWIN)
    if (socketType == SOCK_SEQPACKET)
        socketType = SOCK_DGRAM;
#endif

#if OS(LINUX)
    if ((options & SetCloexecOnServer) || (options & SetCloexecOnClient)) {
        RELEASE_ASSERT(socketpair(AF_UNIX, socketType | SOCK_CLOEXEC, 0, sockets.data()) != -1);

        if (!(options & SetCloexecOnServer))
            RELEASE_ASSERT(unsetCloseOnExec(sockets[1]));
        if (!(options & SetCloexecOnClient))
            RELEASE_ASSERT(unsetCloseOnExec(sockets[0]));

        return { { sockets[0], UnixFileDescriptor::Adopt }, { sockets[1], UnixFileDescriptor::Adopt } };
    }
#endif

    RELEASE_ASSERT(socketpair(AF_UNIX, socketType, 0, sockets.data()) != -1);

#if OS(DARWIN)
    if (socketType == SOCK_DGRAM) {
        sizeDatagramSocket(sockets[0]);
        sizeDatagramSocket(sockets[1]);
    }
#endif

    if (options & SetCloexecOnServer)
        RELEASE_ASSERT(setCloseOnExec(sockets[1]));
    if (options & SetCloexecOnClient)
        RELEASE_ASSERT(setCloseOnExec(sockets[0]));

    return { { sockets[0], UnixFileDescriptor::Adopt }, { sockets[1], UnixFileDescriptor::Adopt } };
}

} // namespace IPC

#endif // USE(UNIX_DOMAIN_SOCKETS)
