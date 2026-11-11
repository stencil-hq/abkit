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

#if HAVE(WEBGPU_IMPLEMENTATION)

#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Seconds.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebCore {

// The work a WebGPU instance schedules for the thread that uses it, when both are in one process.
// The thread runs the work from its run loop. A canvas has to show or read a frame before it
// returns to the run loop, so the thread also runs the work while it waits for the instance, as a
// web process waits for the reply of the GPU process.
class WebGPUWorkQueue final : public ThreadSafeRefCounted<WebGPUWorkQueue> {
public:
    using WorkItem = Function<void()>;

    // The function has the owner thread's run loop call the work item it is given.
    static Ref<WebGPUWorkQueue> create(Function<void(WorkItem&&)>&& scheduleOnOwnerThread)
    {
        return adoptRef(*new WebGPUWorkQueue(WTF::move(scheduleOnOwnerThread)));
    }

    // Can be called on any thread.
    void append(WorkItem&& workItem)
    {
        {
            Locker locker { m_lock };
            m_workItems.append(WTF::move(workItem));
            m_condition.notifyAll();
        }
        m_scheduleOnOwnerThread([protectedThis = Ref { *this }] {
            protectedThis->runPendingWork();
        });
    }

    // The rest are called on the owner thread.
    void runPendingWork()
    {
        while (runNextWorkItem()) { }
    }

    // Runs the work as it arrives until isDone is true. Gives up when the timeout passes with no
    // work to run, which leaves the work to the run loop.
    void runWorkUntil(NOESCAPE const Function<bool()>& isDone, Seconds timeout)
    {
        auto deadline = MonotonicTime::now() + timeout;
        while (!isDone()) {
            if (runNextWorkItem())
                continue;
            Locker locker { m_lock };
            if (!m_condition.waitUntil(m_lock, deadline, [&] WTF_REQUIRES_LOCK(m_lock) { return !m_workItems.isEmpty(); }))
                return;
        }
    }

private:
    explicit WebGPUWorkQueue(Function<void(WorkItem&&)>&& scheduleOnOwnerThread)
        : m_scheduleOnOwnerThread(WTF::move(scheduleOnOwnerThread))
    {
    }

    // One at a time, so that work run from inside a work item keeps the order.
    bool runNextWorkItem()
    {
        WorkItem workItem;
        {
            Locker locker { m_lock };
            if (m_workItems.isEmpty())
                return false;
            workItem = m_workItems.takeFirst();
        }
        workItem();
        return true;
    }

    const Function<void(WorkItem&&)> m_scheduleOnOwnerThread;
    Lock m_lock;
    Condition m_condition;
    Deque<WorkItem> m_workItems WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebCore

#endif // HAVE(WEBGPU_IMPLEMENTATION)
