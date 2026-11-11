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

#if USE(SKIA) && OS(DARWIN)

#include "ScalableImageDecoder.h"

namespace WebCore {

// Decodes HEIC and HEIF stills with ImageIO, as Safari does: the engine tells servers it takes them.
// Of a sequence it decodes the first image.
class HEICImageDecoder final : public ScalableImageDecoder {
public:
    static Ref<ScalableImageDecoder> create(AlphaOption alphaOption, GammaAndColorProfileOption gammaAndColorProfileOption)
    {
        return adoptRef(*new HEICImageDecoder(alphaOption, gammaAndColorProfileOption));
    }

    // ScalableImageDecoder
    String filenameExtension() const final { return "heic"_s; }
    size_t decodeIfNeededAndGetFrameCount() const final { return m_frameCount; }
    RepetitionCount repetitionCount() const final { return RepetitionCountNone; }
    ScalableImageDecoderFrame* frameBufferAtIndex(size_t index) final WTF_REQUIRES_LOCK(m_lock);

private:
    HEICImageDecoder(AlphaOption, GammaAndColorProfileOption);

    void tryDecodeSize(bool allDataReceived) final;
    void decode() WTF_REQUIRES_LOCK(m_lock);

    size_t m_frameCount { 0 };
};

} // namespace WebCore

#endif // USE(SKIA) && OS(DARWIN)
