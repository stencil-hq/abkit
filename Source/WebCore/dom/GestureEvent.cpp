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
 * THIS SOFTWARE IS PROVIDED BY STENCIL ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL STENCIL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GestureEvent.h"

#if ENABLE(MAC_GESTURE_EVENTS) && !PLATFORM(COCOA)

#include "WindowProxy.h"
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GestureEvent);

GestureEvent::GestureEvent()
    : UIEvent(EventInterfaceType::GestureEvent)
{
}

GestureEvent::~GestureEvent() = default;

Ref<GestureEvent> GestureEvent::createForBindings()
{
    return adoptRef(*new GestureEvent);
}

void GestureEvent::initGestureEvent(const AtomString& type, bool bubbles, bool cancelable, RefPtr<WindowProxy>&& view, int detail, double screenX, double screenY, double clientX, double clientY, bool ctrlKey, bool altKey, bool shiftKey, bool metaKey, double scale, double rotation, RefPtr<EventTarget>&& target)
{
    if (isBeingDispatched())
        return;

    UIEvent::initUIEvent(type, bubbles, cancelable, WTF::move(view), detail);

    m_screenX = screenX;
    m_screenY = screenY;
    m_clientX = clientX;
    m_clientY = clientY;
    m_ctrlKey = ctrlKey;
    m_altKey = altKey;
    m_shiftKey = shiftKey;
    m_metaKey = metaKey;
    m_scale = scale;
    m_rotation = rotation;

    setTarget(WTF::move(target));
}

} // namespace WebCore

#endif // ENABLE(MAC_GESTURE_EVENTS) && !PLATFORM(COCOA)
