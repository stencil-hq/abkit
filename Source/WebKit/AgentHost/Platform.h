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

// The few things the host needs from the operating system. Everything that
// differs between platforms is in Platform.cpp.

#include <cstddef>
#include <functional>
#include <string>

namespace AgentHost::Platform {

// The descriptor requests are read from (standard input, in binary mode).
int protocolInput();

// The descriptor answers are written to: a private duplicate of standard
// output. Standard output itself is pointed at standard error afterwards, so
// that nothing a library prints can end up between protocol frames.
// Returns -1 on failure.
int claimProtocolOutput();

// Keeps a write to a closed pipe from ending the process.
void ignoreBrokenPipes();

// Blocking read of up to size bytes. Returns the number of bytes read, 0 at
// the end of the stream and -1 on error.
long readSome(int descriptor, void* buffer, size_t size);

// Blocking write of the whole buffer. Returns false on error.
bool writeAll(int descriptor, const void* buffer, size_t size);

// Calls the function on the main loop when the process is asked to end (SIGTERM, SIGINT, SIGHUP), instead of
// ending it there and then.
void onTerminationRequest(std::function<void()>&&);

// This process's identifier, and whether a process of some identifier is running.
unsigned long processIdentifier();
bool processExists(unsigned long identifier);

// The directory holding this process's executable, or nothing when it cannot be told.
std::string executableDirectory();

} // namespace AgentHost::Platform
