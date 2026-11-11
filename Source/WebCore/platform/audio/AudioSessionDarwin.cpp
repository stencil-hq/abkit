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
#include "AudioSessionDarwin.h"

#if USE(AUDIO_SESSION) && PLATFORM(WPE) && OS(DARWIN)

#include "FloatConversion.h"
#include "Logging.h"
#include <CoreAudio/AudioHardware.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/UniqueArray.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AudioSessionDarwin);

// The default output device is asked for on every query: this port keeps no listener on the
// system's device changes, so a cached device could report one that is no longer the default.
static AudioDeviceID defaultOutputDeviceID()
{
    AudioDeviceID deviceID = kAudioDeviceUnknown;
    UInt32 size = sizeof(deviceID);

    AudioObjectPropertyAddress defaultOutputDeviceAddress = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultOutputDeviceAddress, 0, nullptr, &size, &deviceID))
        return kAudioDeviceUnknown;
    return deviceID;
}

Ref<AudioSessionDarwin> AudioSessionDarwin::create()
{
    return adoptRef(*new AudioSessionDarwin);
}

AudioSessionDarwin::AudioSessionDarwin() = default;
AudioSessionDarwin::~AudioSessionDarwin() = default;

AudioSessionDarwin::CategoryType AudioSessionDarwin::category() const
{
    return m_category;
}

void AudioSessionDarwin::setCategory(CategoryType category, Mode, RouteSharingPolicy policy)
{
    // The device's category is not touched: what WebKit asked for is recorded, so that category()
    // reads back what the Mac session reports after the same request.
    m_category = category;
    m_policy = policy;
}

RouteSharingPolicy AudioSessionDarwin::routeSharingPolicy() const
{
    return m_policy;
}

float AudioSessionDarwin::sampleRate() const
{
    static const AudioObjectPropertyAddress nominalSampleRateAddress = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    Float64 nominalSampleRate = 0;
    UInt32 size = sizeof(nominalSampleRate);
    if (AudioObjectGetPropertyData(defaultOutputDeviceID(), &nominalSampleRateAddress, 0, nullptr, &size, &nominalSampleRate) != noErr) {
        RELEASE_LOG_ERROR(Media, "AudioSessionDarwin::sampleRate() - AudioObjectGetPropertyData() failed, reporting 44100");
        return 44100;
    }

    auto sampleRate = narrowPrecisionToFloat(nominalSampleRate);
    if (!sampleRate) {
        RELEASE_LOG_ERROR(Media, "AudioSessionDarwin::sampleRate() - the default output device reports a sample rate of 0, reporting 44100");
        return 44100;
    }
    return sampleRate;
}

size_t AudioSessionDarwin::bufferSize() const
{
    static const AudioObjectPropertyAddress bufferFrameSizeAddress = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 bufferSize = 0;
    UInt32 size = sizeof(bufferSize);
    if (AudioObjectGetPropertyData(defaultOutputDeviceID(), &bufferFrameSizeAddress, 0, nullptr, &size, &bufferSize))
        return 0;
    return bufferSize;
}

size_t AudioSessionDarwin::preferredBufferSize() const
{
    return m_preferredBufferSize.value_or(bufferSize());
}

void AudioSessionDarwin::setPreferredBufferSize(size_t bufferSize)
{
    if (m_preferredBufferSize == bufferSize)
        return;

    // Only recorded: the device's buffer frame size is never set here. The recorded value is
    // clamped to the range the device offers, so a page reads back the size the Mac session would
    // have applied.
    static const AudioObjectPropertyAddress bufferFrameSizeRangeAddress = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    AudioValueRange bufferSizeRange { 0, 0 };
    UInt32 size = sizeof(bufferSizeRange);
    if (AudioObjectGetPropertyData(defaultOutputDeviceID(), &bufferFrameSizeRangeAddress, 0, nullptr, &size, &bufferSizeRange))
        return;

    m_preferredBufferSize = std::min<size_t>(static_cast<size_t>(bufferSizeRange.mMaximum), std::max<size_t>(static_cast<size_t>(bufferSizeRange.mMinimum), bufferSize));
}

size_t AudioSessionDarwin::maximumNumberOfOutputChannels() const
{
    AudioObjectPropertyAddress streamConfigurationAddress = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(defaultOutputDeviceID(), &streamConfigurationAddress, 0, nullptr, &size) || !size)
        return 0;

    auto listMemory = makeUniqueArray<uint8_t>(size);
    auto* audioBufferList = reinterpret_cast<AudioBufferList*>(listMemory.get());
    if (AudioObjectGetPropertyData(defaultOutputDeviceID(), &streamConfigurationAddress, 0, nullptr, &size, audioBufferList))
        return 0;

    size_t channels = 0;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN; // AudioBufferList is a variable-length struct.
    for (UInt32 i = 0; i < audioBufferList->mNumberBuffers; ++i)
        channels += audioBufferList->mBuffers[i].mNumberChannels;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END;
    return channels;
}

size_t AudioSessionDarwin::outputLatency() const
{
    AudioObjectPropertyAddress latencyAddress = {
        kAudioDevicePropertyLatency,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = sizeof(UInt32);
    UInt32 deviceLatency = 0;
    if (AudioObjectGetPropertyData(defaultOutputDeviceID(), &latencyAddress, 0, nullptr, &size, &deviceLatency) != noErr)
        deviceLatency = 0;

    UInt32 streamLatency = 0;
    latencyAddress.mSelector = kAudioDevicePropertyStreams;
    AudioStreamID streamID;
    size = sizeof(AudioStreamID);
    if (AudioObjectGetPropertyData(defaultOutputDeviceID(), &latencyAddress, 0, nullptr, &size, &streamID) == noErr) {
        latencyAddress.mSelector = kAudioStreamPropertyLatency;
        size = sizeof(UInt32);
        AudioObjectGetPropertyData(streamID, &latencyAddress, 0, nullptr, &size, &streamLatency);
    }

    return deviceLatency + streamLatency;
}

} // namespace WebCore

#endif // USE(AUDIO_SESSION) && PLATFORM(WPE) && OS(DARWIN)
