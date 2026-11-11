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

#if OS(DARWIN) && ENABLE(WPE_PLATFORM)

#include <IOSurface/IOSurfaceRef.h>
#include <sys/types.h>

// The web process renders into IOSurfaces that the UI process hands to the
// embedder. A surface leaves a process as a Mach send right, which the
// socket IPC between the processes cannot carry: each crosses once, when
// its buffer is created, over a Mach port the UI process registers in its
// bootstrap namespace under a name derived from its process ID.
//
// Ordering: the web process sends a surface over the channel before it
// sends the IPC message announcing its buffer, so when the UI process
// handles that message the surface is already queued on its port.
namespace WebKit::IOSurfaceChannel {

// UI process: registers the channel, before any web process starts. False
// when it cannot be registered (web processes then fall back to shared
// memory).
bool listen();

// UI process: the surface web process `pid` sent for buffer `bufferID`,
// retained (the caller releases it), or null.
IOSurfaceRef takeSurface(pid_t, uint64_t bufferID);

// Web process: whether the UI process listens.
bool isAvailable();

// Web process: sends `surface` for buffer `bufferID`.
bool sendSurface(IOSurfaceRef, uint64_t bufferID);

} // namespace WebKit::IOSurfaceChannel

#endif // OS(DARWIN) && ENABLE(WPE_PLATFORM)
