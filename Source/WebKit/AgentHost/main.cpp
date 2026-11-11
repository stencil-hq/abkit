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

#include "Host.h"
#include "Platform.h"
#include <cstdio>
#include <cstring>

// WPEAgentHost [--data-dir DIR]
//
// Runs headless web views and is driven over its standard streams: requests
// on standard input, answers on standard output, both as frames of a 4-byte
// little-endian length and UTF-8 JSON. Diagnostics go to standard error. The
// process ends, with status 0, when standard input does.

int main(int argc, char** argv)
{
    std::string dataDirectory;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data-dir") && i + 1 < argc)
            dataDirectory = argv[++i];
        else if (!strncmp(argv[i], "--data-dir=", 11))
            dataDirectory = argv[i] + 11;
        else {
            fprintf(stderr, "usage: %s [--data-dir DIR]\n", argv[0]);
            return 64;
        }
    }

    // From here on only the host writes to the stream its parent reads.
    AgentHost::Platform::ignoreBrokenPipes();
    int input = AgentHost::Platform::protocolInput();
    int output = AgentHost::Platform::claimProtocolOutput();
    if (output < 0) {
        fprintf(stderr, "%s: cannot take over standard output\n", argv[0]);
        return 1;
    }
    // What still goes to standard output is diagnostics now: seen as soon as written, as on standard error.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_log_writer_default_set_use_stderr(TRUE);

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    int status = 0;
    AgentHost::Host host(std::move(dataDirectory), [loop, &status](int exitStatus) {
        status = exitStatus;
        g_main_loop_quit(loop);
    });
    if (!host.start(input, output))
        return 1;
    // Asked to end, the host ends as it does when its input does, with the status of a process a signal ended.
    AgentHost::Platform::onTerminationRequest([loop, &status] {
        status = 143;
        g_main_loop_quit(loop);
    });
    g_main_loop_run(loop);
    host.stop();
    // The engine's helper processes end when this one does.
    return status;
}
