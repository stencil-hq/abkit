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

#import "config.h"
#import "MediaTypeSupportDarwin.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)

#import "ContentType.h"
#import "VP9Utilities.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/ps/IOPowerSources.h>
#import <VideoToolbox/VideoToolbox.h>
#import <dlfcn.h>
#import <mutex>
#import <wtf/HashMap.h>
#import <wtf/Lock.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/SortedArrayMap.h>
#import <wtf/text/MakeString.h>
#import <wtf/text/StringHash.h>

// Neither is in the SDK's headers: the form of the playability question that takes options, and the
// parser behind the Cocoa ports' media source buffers. The class is looked up by name, so nothing here
// needs it to exist.
@interface AVURLAsset (WebCoreExtendedPlayability)
+ (BOOL)isPlayableExtendedMIMEType:(NSString *)extendedMIMEType options:(NSDictionary<NSString *, id> *)options;
@end

@interface AVStreamDataParser : NSObject
+ (NSArray<NSString *> *)audiovisualMIMETypes;
+ (BOOL)canParseExtendedMIMEType:(NSString *)extendedMIMEType;
+ (NSString *)outputMIMECodecParameterForInputMIMECodecParameter:(NSString *)inputMIMECodecParameter;
@end

namespace WebCore {
namespace MediaTypeSupportDarwin {

using SupportsType = MediaPlayerEnums::SupportsType;

// AudioToolbox's list of the channel layouts a format decodes to; the SDK names only the encoding one.
static constexpr AudioFormatPropertyID availableDecodeChannelLayoutTagsProperty = 'adcl';

static Class streamDataParserClass()
{
    static Class parser = [] {
        Class candidate = NSClassFromString(@"AVStreamDataParser");
        return [candidate respondsToSelector:@selector(audiovisualMIMETypes)] ? candidate : nil;
    }();
    return parser;
}

static bool hasMediaPrefix(const String& containerType)
{
    return startsWithLettersIgnoringASCIICase(containerType, "video/"_s)
        || startsWithLettersIgnoringASCIICase(containerType, "audio/"_s)
        || startsWithLettersIgnoringASCIICase(containerType, "application/"_s);
}

static bool isMP3InMPEGAudio(const ContentType& type)
{
    // AVFoundation does not know the codec string, which pages do use.
    auto codecs = type.codecs();
    return equalLettersIgnoringASCIICase(type.containerType(), "audio/mpeg"_s) && codecs.size() == 1 && codecs[0] == "mp3"_s;
}

static bool hardwareDecodes(CMVideoCodecType codec)
{
    return VTIsHardwareDecodeSupported(codec);
}

static void registerSupplementalDecoders()
{
    // The system's VP9 decoder is a supplemental one, known to VideoToolbox and to AVFoundation's
    // answers once a process asks for it, as WebKit does before it asks anything.
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
    });
}

static bool hardwareDecodesVP9()
{
    registerSupplementalDecoders();
    static bool result = hardwareDecodes(kCMVideoCodecType_VP9);
    return result;
}

static bool anyPowerSource(CFStringRef key, CFStringRef value, bool matchesWhenAbsent)
{
    RetainPtr info = adoptCF(IOPSCopyPowerSourcesInfo());
    if (!info)
        return false;
    RetainPtr sources = adoptCF(IOPSCopyPowerSourcesList(info.get()));
    if (!sources)
        return false;
    for (CFIndex i = 0, count = CFArrayGetCount(sources.get()); i < count; ++i) {
        CFDictionaryRef description = IOPSGetPowerSourceDescription(info.get(), CFArrayGetValueAtIndex(sources.get(), i));
        if (!description)
            continue;
        CFTypeRef found = CFDictionaryGetValue(description, key);
        if (found ? CFEqual(found, value) : matchesWhenAbsent)
            return true;
    }
    return false;
}

static bool hasScreenFor4K()
{
    uint32_t count = 0;
    std::array<CGDirectDisplayID, 16> displays;
    if (CGGetActiveDisplayList(displays.size(), displays.data(), &count) != kCGErrorSuccess)
        return false;
    for (auto display : std::span { displays }.first(count)) {
        RetainPtr mode = adoptCF(CGDisplayCopyDisplayMode(display));
        if (mode && CGDisplayModeGetPixelWidth(mode.get()) * CGDisplayModeGetPixelHeight(mode.get()) > 2048 * 1080)
            return true;
    }
    return false;
}

static bool supportsVPConfiguration(const VPCodecConfigurationRecord& configuration)
{
    bool isVP9 = configuration.codecName == "vp09"_s || configuration.codecName == "vp9"_s;
    if (!isVP9) {
        // The VP8 decoder has profile 0, 4:2:0 and 8 bits.
        return !configuration.profile && configuration.chromaSubsampling <= VPConfigurationChromaSubsampling::Subsampling_420_Colocated && configuration.bitDepth <= 8;
    }

    static bool hasBattery = anyPowerSource(CFSTR(kIOPSTypeKey), CFSTR(kIOPSInternalBatteryType), true);
    // A Mac on battery without a hardware decoder has no VP9 decoder at all.
    if (!hardwareDecodesVP9() && hasBattery)
        return false;

    if ((configuration.profile && configuration.profile != 2)
        || configuration.chromaSubsampling > VPConfigurationChromaSubsampling::Subsampling_420_Colocated
        || configuration.bitDepth > 10
        || configuration.level > VPConfigurationLevel::Level_6)
        return false;

    // Without a hardware decoder VP9 is offered where power does not matter, or where a screen is
    // large enough for the 4K video that only VP9 brings.
    return hardwareDecodesVP9() || !hasBattery || anyPowerSource(CFSTR(kIOPSPowerSourceStateKey), CFSTR(kIOPSACPowerValue), false) || hasScreenFor4K();
}

// WebM is WebKit's own parser on a Mac, and so are these rules.
static SupportsType supportsWebM(const ContentType& type)
{
    auto containerType = type.containerType();
    if (!equalLettersIgnoringASCIICase(containerType, "audio/webm"_s) && !equalLettersIgnoringASCIICase(containerType, "video/webm"_s))
        return SupportsType::IsNotSupported;

    auto codecs = type.codecs();
    if (codecs.isEmpty())
        return SupportsType::MayBeSupported;

    for (auto& codec : codecs) {
        if (codec.startsWith("vp09"_s) || codec.startsWith("vp08"_s) || codec == "vp9"_s || codec == "vp9.0"_s || codec == "vp8"_s || codec == "vp8.0"_s) {
            auto configuration = parseVPCodecParameters(codec);
            if (!configuration || !supportsVPConfiguration(*configuration))
                return SupportsType::IsNotSupported;
            continue;
        }
        if (codec == "vorbis"_s || codec == "opus"_s)
            continue;
        if (codec.startsWith("av01."_s) && hardwareDecodes(kCMVideoCodecType_AV1))
            continue;
        return SupportsType::IsNotSupported;
    }
    return SupportsType::IsSupported;
}

static bool decodesMultichannelOpus()
{
    static bool result = [] {
        AudioStreamBasicDescription description { };
        description.mFormatID = kAudioFormatOpus;
        UInt32 size = 0;
        if (AudioFormatGetPropertyInfo(availableDecodeChannelLayoutTagsProperty, sizeof(description), &description, &size) != noErr || size < sizeof(AudioChannelLayoutTag))
            return false;
        Vector<AudioChannelLayoutTag> tags;
        tags.fill(0, size / sizeof(AudioChannelLayoutTag));
        if (AudioFormatGetProperty(availableDecodeChannelLayoutTagsProperty, sizeof(description), &description, &size, tags.mutableSpan().data()) != noErr)
            return false;
        for (auto tag : tags) {
            auto layout = tag & 0xFFFF0000;
            if (layout != kAudioChannelLayoutTag_Unknown && layout != kAudioChannelLayoutTag_DiscreteInOrder && AudioChannelLayoutTag_GetNumberOfChannels(tag) > 2)
                return true;
        }
        return false;
    }();
    return result;
}

static const HashSet<String>& assetContainerTypes()
{
    static NeverDestroyed<HashSet<String>> types = [] {
        HashSet<String> result;
        @autoreleasepool {
            for (NSString *type in [AVURLAsset audiovisualMIMETypes])
                result.add(String(type));
        }
        return result;
    }();
    return types;
}

static const HashSet<String>& parserContainerTypes()
{
    static NeverDestroyed<HashSet<String>> types = [] {
        HashSet<String> result;
        @autoreleasepool {
            for (NSString *type in [streamDataParserClass() audiovisualMIMETypes])
                result.add(String(type));
        }
        return result;
    }();
    return types;
}

// The types WebKit takes without asking AVFoundation, whatever their case.
static constexpr auto staticAssetContainerTypes()
{
    return WTF::toArray<ComparableLettersLiteral>({
        "application/vnd.apple.mpegurl"_s,
        "application/x-mpegurl"_s,
        "audio/3gpp"_s,
        "audio/aac"_s,
        "audio/aacp"_s,
        "audio/aiff"_s,
        "audio/basic"_s,
        "audio/mp3"_s,
        "audio/mp4"_s,
        "audio/mpeg"_s,
        "audio/mpeg3"_s,
        "audio/mpegurl"_s,
        "audio/mpg"_s,
        "audio/vnd.wave"_s,
        "audio/wav"_s,
        "audio/wave"_s,
        "audio/x-aac"_s,
        "audio/x-aiff"_s,
        "audio/x-m4a"_s,
        "audio/x-mpegurl"_s,
        "audio/x-wav"_s,
        "video/3gpp"_s,
        "video/3gpp2"_s,
        "video/mp4"_s,
        "video/mpeg"_s,
        "video/mpeg2"_s,
        "video/mpg"_s,
        "video/quicktime"_s,
        "video/x-m4v"_s,
        "video/x-mpeg"_s,
        "video/x-mpg"_s,
    });
}

static bool isPlayableByAsset(const ContentType& type)
{
    auto containerType = type.containerType();
    bool isMP4 = containerType == "video/mp4"_s || containerType == "audio/mp4"_s;
    auto codecs = type.codecs();
    if (isMP4 && codecs.contains("opus"_s) && !decodesMultichannelOpus())
        return false;

    // AVFoundation knows FLAC in MP4 by its registered name only.
    String extendedType = isMP4 && codecs.contains("flac"_s) ? makeStringByReplacingAll(type.raw(), "flac"_s, "fLaC"_s) : type.raw();
    registerSupplementalDecoders();

    @autoreleasepool {
        // With this option a playlist type's codecs are judged as those of its segments.
        static NSString *playlistOption = [] {
            auto* symbol = static_cast<NSString * const *>(dlsym(RTLD_DEFAULT, "AVURLAssetExtendedMIMETypePlayabilityTreatPlaylistMIMETypesAsISOBMFFMediaDataContainersKey"));
            return symbol ? *symbol : nil;
        }();
        if (playlistOption)
            return [AVURLAsset isPlayableExtendedMIMEType:extendedType.createNSString().get() options:@{ playlistOption: @YES }];
        return [AVURLAsset isPlayableExtendedMIMEType:extendedType.createNSString().get()];
    }
}

static SupportsType supportsAssetType(const ContentType& type)
{
    auto containerType = type.containerType();
    if (containerType.isEmpty() || !hasMediaPrefix(containerType)
        || equalLettersIgnoringASCIICase(containerType, "video/h264"_s) || equalLettersIgnoringASCIICase(containerType, "video/x-flv"_s))
        return SupportsType::IsNotSupported;

    static constexpr SortedArraySet staticTypes { staticAssetContainerTypes() };
    if (!staticTypes.contains(StringView { containerType }) && !assetContainerTypes().contains(containerType))
        return SupportsType::IsNotSupported;

    if (type.codecs().isEmpty())
        return SupportsType::MayBeSupported;

    return isMP3InMPEGAudio(type) || isPlayableByAsset(type) ? SupportsType::IsSupported : SupportsType::IsNotSupported;
}

static SupportsType supportsParserType(const ContentType& type)
{
    Class parser = streamDataParserClass();
    if (!parser)
        return supportsAssetType(type);

    registerSupplementalDecoders();
    @autoreleasepool {
        // The parser names some codecs differently from pages (avc3 is avc1 to it, hev1 is hvc1).
        ContentType extendedType = type;
        auto codecsParameter = type.parameter(ContentType::codecsParameter());
        if (!codecsParameter.isEmpty()) {
            String parserCodecs = [parser outputMIMECodecParameterForInputMIMECodecParameter:codecsParameter.createNSString().get()];
            extendedType = ContentType(makeString(type.containerType(), "; codecs=\""_s, parserCodecs, '"'));
        }

        auto containerType = extendedType.containerType();
        if (containerType.isEmpty() || !parserContainerTypes().contains(containerType))
            return SupportsType::IsNotSupported;

        if (extendedType.codecs().isEmpty())
            return SupportsType::MayBeSupported;

        if (isMP3InMPEGAudio(extendedType) || [parser canParseExtendedMIMEType:extendedType.raw().createNSString().get()])
            return SupportsType::IsSupported;
        return SupportsType::IsNotSupported;
    }
}

// Pages ask the same few questions over and over, and AVFoundation takes its time for each.
static SupportsType cachedAnswer(HashMap<String, SupportsType>& cache, const ContentType& type, NOESCAPE const Function<SupportsType()>& compute)
{
    static Lock lock;
    {
        Locker locker { lock };
        if (auto found = cache.getOptional(type.raw()))
            return *found;
    }
    auto answer = compute();
    Locker locker { lock };
    cache.set(type.raw().isolatedCopy(), answer);
    return answer;
}

SupportsType supportsFileType(const ContentType& type)
{
    if (type.isEmpty())
        return SupportsType::IsNotSupported;

    static NeverDestroyed<HashMap<String, SupportsType>> cache;
    return cachedAnswer(cache, type, [&] {
        // WebKit has one player for what AVFoundation plays and one for WebM, and asks the one
        // that answers best, where "maybe" counts for more than "probably".
        auto asset = supportsAssetType(type);
        auto webM = supportsWebM(type);
        return webM > asset ? webM : asset;
    });
}

SupportsType supportsMediaSourceType(const ContentType& type)
{
    if (type.isEmpty())
        return SupportsType::IsNotSupported;

    static NeverDestroyed<HashMap<String, SupportsType>> cache;
    return cachedAnswer(cache, type, [&] {
        auto webM = supportsWebM(type);
        if (webM == SupportsType::IsSupported)
            return webM;
        return std::max(webM, supportsParserType(type));
    });
}

void getSupportedFileTypes(HashSet<String>& types)
{
    types = assetContainerTypes();
    for (auto& type : staticAssetContainerTypes())
        types.add(String { type.literal });
    types.add("video/webm"_s);
    types.add("audio/webm"_s);
}

void getSupportedMediaSourceTypes(HashSet<String>& types)
{
    if (!streamDataParserClass()) {
        getSupportedFileTypes(types);
        return;
    }
    types = parserContainerTypes();
    types.add("video/webm"_s);
    types.add("audio/webm"_s);
}

} // namespace MediaTypeSupportDarwin
} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(GSTREAMER) && OS(DARWIN)
