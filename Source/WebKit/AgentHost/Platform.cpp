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

#include "Platform.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <glib.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <glib-unix.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace AgentHost::Platform {

#if defined(_WIN32)

// Windows. Not built or run yet: the POSIX half below is what is tested.

int protocolInput()
{
    _setmode(0, _O_BINARY);
    return 0;
}

int claimProtocolOutput()
{
    fflush(stdout);
    int output = _dup(1);
    if (output < 0)
        return -1;
    _setmode(output, _O_BINARY);
    if (_dup2(2, 1) < 0) {
        _close(output);
        return -1;
    }
    return output;
}

void ignoreBrokenPipes()
{
    // A write to a closed pipe fails with EPIPE here; no signal is raised.
}

long readSome(int descriptor, void* buffer, size_t size)
{
    unsigned count = size > 0x40000000u ? 0x40000000u : static_cast<unsigned>(size);
    return _read(descriptor, buffer, count);
}

bool writeAll(int descriptor, const void* buffer, size_t size)
{
    auto* bytes = static_cast<const char*>(buffer);
    while (size) {
        unsigned count = size > 0x40000000u ? 0x40000000u : static_cast<unsigned>(size);
        int written = _write(descriptor, bytes, count);
        if (written <= 0)
            return false;
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

void onTerminationRequest(std::function<void()>&&)
{
    // Not done here: a parent that wants the host to end closes its standard input.
}

unsigned long processIdentifier()
{
    return GetCurrentProcessId();
}

bool processExists(unsigned long identifier)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identifier);
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD status = 0;
    bool running = GetExitCodeProcess(process, &status) && status == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

std::string executableDirectory()
{
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(nullptr, path, G_N_ELEMENTS(path));
    if (!length || length >= G_N_ELEMENTS(path))
        return { };
    char* utf8 = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(path), length, nullptr, nullptr, nullptr);
    if (!utf8)
        return { };
    char* directory = g_path_get_dirname(utf8);
    std::string result = directory;
    g_free(directory);
    g_free(utf8);
    return result;
}

#else

// POSIX (macOS, Linux).

int protocolInput()
{
    return STDIN_FILENO;
}

int claimProtocolOutput()
{
    fflush(stdout);
    int output = dup(STDOUT_FILENO);
    if (output < 0)
        return -1;
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        close(output);
        return -1;
    }
    // The engine's helper processes must not inherit the protocol stream.
    fcntl(output, F_SETFD, FD_CLOEXEC);
    return output;
}

void ignoreBrokenPipes()
{
    signal(SIGPIPE, SIG_IGN);
}

long readSome(int descriptor, void* buffer, size_t size)
{
    while (true) {
        ssize_t count = read(descriptor, buffer, size);
        if (count < 0 && errno == EINTR)
            continue;
        return static_cast<long>(count);
    }
}

bool writeAll(int descriptor, const void* buffer, size_t size)
{
    auto* bytes = static_cast<const char*>(buffer);
    while (size) {
        ssize_t written = write(descriptor, bytes, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

void onTerminationRequest(std::function<void()>&& function)
{
    auto* shared = new std::function<void()>(std::move(function));
    for (int signal : { SIGTERM, SIGINT, SIGHUP }) {
        g_unix_signal_add(signal, [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_REMOVE;
        }, shared);
    }
}

unsigned long processIdentifier()
{
    return static_cast<unsigned long>(getpid());
}

bool processExists(unsigned long identifier)
{
    return !kill(static_cast<pid_t>(identifier), 0) || errno == EPERM;
}

std::string executableDirectory()
{
    char path[PATH_MAX];
#if defined(__APPLE__)
    char given[PATH_MAX];
    uint32_t size = sizeof(given);
    if (_NSGetExecutablePath(given, &size) || !realpath(given, path))
        return { };
#else
    // Linux; the other systems that have /proc name the link differently and get no directory.
    if (!realpath("/proc/self/exe", path))
        return { };
#endif
    char* directory = g_path_get_dirname(path);
    std::string result = directory;
    g_free(directory);
    return result;
}

#endif

} // namespace AgentHost::Platform
