/*
 * Copyright (C) 2017 Igalia S.L. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LibWebRTCProviderGStreamer.h"

#if USE(LIBWEBRTC) && USE(GSTREAMER)

#if OS(DARWIN)
#include "LibWebRTCDav1dDecoder.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/darwin/WeakLinking.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <webrtc/api/create_modular_peer_connection_factory.h>
#include <webrtc/webkit_sdk/WebKit/WebKitDecoder.h>
#include <webrtc/webkit_sdk/WebKit/WebKitEncoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

WTF_WEAK_LINK_FORCE_IMPORT(webrtc::CreateModularPeerConnectionFactory);
#endif

namespace WebCore {

UniqueRef<WebRTCProvider> WebRTCProvider::create()
{
    return makeUniqueRef<LibWebRTCProviderGStreamer>();
}

bool WebRTCProvider::webRTCAvailable()
{
#if OS(DARWIN)
    // libwebrtc is a library of its own there, linked weakly.
    return !!webrtc::CreateModularPeerConnectionFactory;
#else
    return true;
#endif
}

#if OS(DARWIN)
// On Darwin the codecs are the ones libwebrtc is built with for the Cocoa ports (VideoToolbox
// for H.264 and H.265, libvpx for VP8 and VP9, libaom and dav1d for AV1), so that the formats
// offered and the bitstreams produced are those of Safari on the same machine. GStreamer only
// carries the frames to and from the tracks.

#if ENABLE(AV1)
// libwebrtc's factory offers AV1 and leaves the decoder to WebKit, which the Cocoa ports give
// it from the GPU process connection. Here it is WebCore's dav1d decoder, the one they use
// when the machine has no hardware decoder.
class DecoderFactoryWithAV1 final : public webrtc::VideoDecoderFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DecoderFactoryWithAV1);
public:
    explicit DecoderFactoryWithAV1(std::unique_ptr<webrtc::VideoDecoderFactory>&& factory)
        : m_factory(WTF::move(factory))
    {
    }

private:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const final { return m_factory->GetSupportedFormats(); }
    CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format, bool referenceScaling, std::optional<webrtc::Resolution> resolution) const final { return m_factory->QueryCodecSupport(format, referenceScaling, resolution); }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format) final
    {
        if (format.name == "AV1")
            return createLibWebRTCDav1dDecoder().moveToUniquePtr();
        return m_factory->Create(environment, format);
    }

    std::unique_ptr<webrtc::VideoDecoderFactory> m_factory;
};
#endif

std::unique_ptr<webrtc::VideoDecoderFactory> LibWebRTCProviderGStreamer::createDecoderFactory()
{
    auto vp9Support = isSupportingVP9Profile2() ? webrtc::WebKitVP9::Profile0And2 : isSupportingVP9Profile0() ? webrtc::WebKitVP9::Profile0 : webrtc::WebKitVP9::Off;
    auto factory = webrtc::createWebKitDecoderFactory(isSupportingH265() ? webrtc::WebKitH265::On : webrtc::WebKitH265::Off, vp9Support, isSupportingAV1() ? webrtc::WebKitAv1::On : webrtc::WebKitAv1::Off);
#if ENABLE(AV1)
    if (isSupportingAV1())
        return makeUnique<DecoderFactoryWithAV1>(WTF::move(factory));
#endif
    return factory;
}

std::unique_ptr<webrtc::VideoEncoderFactory> LibWebRTCProviderGStreamer::createEncoderFactory()
{
    auto vp9Support = isSupportingVP9Profile2() ? webrtc::WebKitVP9::Profile0And2 : isSupportingVP9Profile0() ? webrtc::WebKitVP9::Profile0 : webrtc::WebKitVP9::Off;
    return webrtc::createWebKitEncoderFactory(isSupportingH265() ? webrtc::WebKitH265::On : webrtc::WebKitH265::Off, vp9Support, isSupportingAV1() ? webrtc::WebKitAv1::On : webrtc::WebKitAv1::Off);
}
#else
std::unique_ptr<webrtc::VideoDecoderFactory> LibWebRTCProviderGStreamer::createDecoderFactory()
{
    return makeUnique<GStreamerVideoDecoderFactory>(isSupportingVP9Profile0(), isSupportingVP9Profile2());
}

std::unique_ptr<webrtc::VideoEncoderFactory> LibWebRTCProviderGStreamer::createEncoderFactory()
{
    return makeUnique<GStreamerVideoEncoderFactory>(isSupportingVP9Profile0(), isSupportingVP9Profile2());
}
#endif

} // namespace WebCore

#endif // USE(LIBWEBRTC) && USE(GSTREAMER)
