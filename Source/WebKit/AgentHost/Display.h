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

// The WPE Platform display the host's web views are made on. It has no
// window system behind it: a view is a size, a scale, a state (mapped,
// visible, in an active toplevel, focused) and a sink for the frames the
// page renders, which go nowhere unless somebody asked for one or a client
// is shown them. Its one screen paces the pages: by the ticks of a client's
// display while one comes in, else at 60 Hz.

#include <functional>
#include <string>
#include <wpe/wpe-platform.h>

namespace AgentHost {

// A new display, connected and made the primary one (the engine reads its settings there).
WPEDisplay* createDisplay();

// The size, in CSS pixels, of the views the display creates from now on.
void setNewViewSize(WPEDisplay*, int width, int height);

// What is called with each frame a view of the display renders, while the buffer is still the view's. It
// returns whether it keeps the buffer: then it calls wpe_view_buffer_rendered() and
// wpe_view_buffer_released() for it itself; else the frame counts as shown and released at once.
using FrameHandler = std::function<bool(WPEBuffer*)>;
void setFrameHandler(WPEView*, FrameHandler&&);

// What is called with the name of the cursor (a CSS keyword) the page shows over a view.
using CursorHandler = std::function<void(const char*)>;
void setCursorHandler(WPEView*, CursorHandler&&);

// Gives a view of the display, and its toplevel, a new size.
void resizeView(WPEView*, int width, int height);

// Gives a view of the display, and its toplevel, a new scale (device pixels per CSS pixel).
void setViewScale(WPEView*, double scale);

// The client's display refreshed: the display's screen paces the pages by these while they come. From any
// thread.
void displayTicked();

// Runs a function on the main loop after the screen's next refresh (a client's tick, or the timer's).
void afterNextRefresh(std::function<void()>&&);

} // namespace AgentHost
