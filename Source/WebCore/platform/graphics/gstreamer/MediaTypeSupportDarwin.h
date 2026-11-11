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

#if ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)

#include "MediaPlayerEnums.h"
#include <wtf/Forward.h>
#include <wtf/HashSet.h>

namespace WebCore {

class ContentType;

// What the system's WebKit answers on this Mac for the types a page asks about: the Cocoa ports put
// the question to AVFoundation and add rules of their own for WebM, and this does the same, so that
// canPlayType() and MediaSource.isTypeSupported() agree with Safari on the same machine for any
// string, including the ones Safari answers oddly. GStreamer plays the media either way.
namespace MediaTypeSupportDarwin {

// A media element's source (a file or an HLS playlist).
MediaPlayerEnums::SupportsType supportsFileType(const ContentType&);
void getSupportedFileTypes(HashSet<String>&);

// A MediaSource's source buffer.
MediaPlayerEnums::SupportsType supportsMediaSourceType(const ContentType&);
void getSupportedMediaSourceTypes(HashSet<String>&);

} // namespace MediaTypeSupportDarwin

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)
