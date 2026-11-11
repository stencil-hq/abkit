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

#pragma once

#include "EventTarget.h"
#include "UIEvent.h"
#include <wtf/RefPtr.h>

#if ENABLE(MAC_GESTURE_EVENTS) && !PLATFORM(COCOA)

namespace WebCore {

class WindowProxy;

// The trackpad gesture event interface. Gesture events are only produced by
// AppKit, so this interface is compiled without a producer outside Cocoa.
class GestureEvent final : public UIEvent {
    WTF_MAKE_TZONE_ALLOCATED(GestureEvent);
public:
    static Ref<GestureEvent> createForBindings();

    virtual ~GestureEvent();

    double screenX() const override { return m_screenX; }
    double screenY() const override { return m_screenY; }
    double clientX() const override { return m_clientX; }
    double clientY() const override { return m_clientY; }
    bool ctrlKey() const { return m_ctrlKey; }
    bool shiftKey() const { return m_shiftKey; }
    bool altKey() const { return m_altKey; }
    bool metaKey() const { return m_metaKey; }
    double scale() const { return m_scale; }
    double rotation() const { return m_rotation; }

    void initGestureEvent(const AtomString& type, bool bubbles, bool cancelable, RefPtr<WindowProxy>&& view, int detail, double screenX, double screenY, double clientX, double clientY, bool ctrlKey, bool altKey, bool shiftKey, bool metaKey, double scale, double rotation, RefPtr<EventTarget>&& target);

private:
    GestureEvent();

    double m_screenX { 0 };
    double m_screenY { 0 };
    double m_clientX { 0 };
    double m_clientY { 0 };
    bool m_ctrlKey { false };
    bool m_shiftKey { false };
    bool m_altKey { false };
    bool m_metaKey { false };
    double m_scale { 0 };
    double m_rotation { 0 };
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_EVENT(GestureEvent)

#endif // ENABLE(MAC_GESTURE_EVENTS) && !PLATFORM(COCOA)
