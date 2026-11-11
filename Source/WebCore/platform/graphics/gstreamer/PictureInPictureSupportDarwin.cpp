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

#include "config.h"

#if ENABLE(VIDEO_PRESENTATION_MODE)

#include "HTMLMediaElement.h"
#include "MediaPlayer.h"
#include "PictureInPictureSupport.h"

namespace WebCore {

// The Mac has a Picture-in-Picture window to give a video, but this engine has
// no window at all, and no video element here reports the Picture-in-Picture
// presentation mode as supported. The API is present, as it is in Safari, and
// reports the same capability; requests that would need a window end the way
// Safari ends them when no window is available.
static bool s_supportsPictureInPicture { true };

bool supportsPictureInPicture()
{
    return s_supportsPictureInPicture;
}

void setSupportsPictureInPicture(bool supports)
{
    s_supportsPictureInPicture = supports;
}

void HTMLMediaElement::updatePlayerVideoFullscreenLayer(MediaPlayer& player)
{
    // Without a window there is no fullscreen video layer to give the player.
    player.setVideoFullscreenLayer(m_videoFullscreenLayer.get());
}

} // namespace WebCore

#endif // ENABLE(VIDEO_PRESENTATION_MODE)
