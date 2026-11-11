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

#include <wtf/Platform.h>
#if USE(AUDIO_SESSION) && PLATFORM(WPE) && OS(DARWIN)

#include <WebCore/AudioSession.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

// The hardware questions Web Audio and navigator.audioSession ask (sample rate, buffer size,
// channels, output latency) are put to the default output device through CoreAudio, as the Cocoa
// port's Mac session puts them. The device's configuration is never changed: this engine plays no
// sound unless the embedder asks for it, so a buffer size handed to setPreferredBufferSize() is
// only recorded and reported back.
class AudioSessionDarwin final : public AudioSession, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<AudioSessionDarwin> {
    WTF_MAKE_TZONE_ALLOCATED(AudioSessionDarwin);
public:
    static Ref<AudioSessionDarwin> create();
    virtual ~AudioSessionDarwin();

    WTF_ABSTRACT_THREAD_SAFE_REF_COUNTED_AND_CAN_MAKE_WEAK_PTR_IMPL;

private:
    AudioSessionDarwin();

    // AudioSession. numberOfOutputChannels() is not overridden: the Mac session answers 0 for it,
    // as the base class does.
    CategoryType category() const final;
    void setCategory(CategoryType, Mode, RouteSharingPolicy) final;
    RouteSharingPolicy routeSharingPolicy() const final;
    float sampleRate() const final;
    size_t bufferSize() const final;
    size_t maximumNumberOfOutputChannels() const final;
    size_t preferredBufferSize() const final;
    void setPreferredBufferSize(size_t) final;
    size_t outputLatency() const final;

    CategoryType m_category { CategoryType::None };
    RouteSharingPolicy m_policy { RouteSharingPolicy::Default };
    std::optional<size_t> m_preferredBufferSize;
};

} // namespace WebCore

#endif // USE(AUDIO_SESSION) && PLATFORM(WPE) && OS(DARWIN)
