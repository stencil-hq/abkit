/*
 * Copyright (C) 2023 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "DisplayVBlankMonitorTimer.h"

#include <WebCore/AnimationFrameRate.h>
#include <chrono>
#include <thread>

namespace WebKit {

std::unique_ptr<DisplayVBlankMonitor> DisplayVBlankMonitorTimer::create()
{
    return makeUnique<DisplayVBlankMonitorTimer>();
}

DisplayVBlankMonitorTimer::DisplayVBlankMonitorTimer()
    : DisplayVBlankMonitorThreaded(WebCore::FullSpeedFramesPerSecond)
{
}

bool DisplayVBlankMonitorTimer::waitForVBlank() const
{
    // Tick on a fixed grid, as a display does. Sleeping a whole interval after each tick adds the time
    // every frame took to handle to the period, and the page sees a refresh rate no screen has.
    using Clock = std::chrono::steady_clock;
    auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / m_refreshRate));
    auto now = Clock::now();
    if (m_nextTick == Clock::time_point { })
        m_nextTick = now;
    do {
        m_nextTick += interval;
    } while (m_nextTick <= now);

    std::this_thread::sleep_until(m_nextTick);
    return true;
}

} // namespace WebKit
