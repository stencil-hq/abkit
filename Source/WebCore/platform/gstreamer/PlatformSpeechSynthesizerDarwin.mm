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
#import "PlatformSpeechSynthesizer.h"

#if ENABLE(SPEECH_SYNTHESIS) && PLATFORM(WPE) && OS(DARWIN)

#import "PlatformSpeechSynthesisUtterance.h"
#import "PlatformSpeechSynthesisVoice.h"

#if __has_include(<AVFAudio/AVSpeechSynthesis.h>)
#import <AVFAudio/AVSpeechSynthesis.h>
#else
#import <AVFoundation/AVFoundation.h>
#endif

#import <dlfcn.h>
#import <wtf/Assertions.h>
#import <wtf/BlockObjCExceptions.h>
#import <wtf/Compiler.h>
#import <wtf/MainThread.h>
#import <wtf/Ref.h>
#import <wtf/RefPtr.h>
#import <wtf/RetainPtr.h>
#import <wtf/StdLibExtras.h>
#import <wtf/WeakPtr.h>

// The super compact voices, whether a voice is a system one and the notification about a change of
// the list are private API, as they are to the system's WebKit (AXSpeechManagerSPI.h).
@interface AVSpeechSynthesisVoice (WebKitSpeechSynthesisVoiceList)
@property (nonatomic, readonly) BOOL isSystemVoice;
+ (NSArray<AVSpeechSynthesisVoice *> *)speechVoicesIncludingSuperCompact;
+ (void)speechVoicesIncludingSuperCompactWithCompletionHandler:(void (^)(NSArray<AVSpeechSynthesisVoice *> *))completion;
@end

// AVFAudio's headers declare the rate constants and the notification name as plain variables, which
// a C++ translation unit cannot refer to by name, so they are looked up at run time as PAL's soft
// linking does for the system's WebKit.
static void* speechSynthesisSymbol(const char* name)
{
    void* symbol = dlsym(RTLD_DEFAULT, name);
    RELEASE_ASSERT_WITH_MESSAGE(symbol, "%s", dlerror());
    return symbol;
}

static float avSpeechUtteranceDefaultSpeechRate()
{
    static float value = *static_cast<float const*>(speechSynthesisSymbol("AVSpeechUtteranceDefaultSpeechRate"));
    return value;
}

static float avSpeechUtteranceMaximumSpeechRate()
{
    static float value = *static_cast<float const*>(speechSynthesisSymbol("AVSpeechUtteranceMaximumSpeechRate"));
    return value;
}

static NSNotificationName avSpeechSynthesisAvailableVoicesDidChangeNotification()
{
    static NSString *value = *static_cast<NSString *const*>(speechSynthesisSymbol("AVSpeechSynthesisAvailableVoicesDidChangeNotification"));
    return value;
}

@interface WebSpeechSynthesisWrapper : NSObject<AVSpeechSynthesizerDelegate> {
    WeakPtr<WebCore::PlatformSpeechSynthesizer> m_synthesizerObject;
    // Hold a Ref to the utterance so that it won't disappear until the synth is done with it.
    RefPtr<WebCore::PlatformSpeechSynthesisUtterance> m_utterance;
    // The AVFoundation utterance being spoken, to tell our own delegate callbacks from stale ones.
    RetainPtr<AVSpeechUtterance> m_avUtterance;

    const RetainPtr<AVSpeechSynthesizer> m_synthesizer;
}

- (WebSpeechSynthesisWrapper *)initWithSpeechSynthesizer:(WebCore::PlatformSpeechSynthesizer*)synthesizer;
- (void)speakUtterance:(RefPtr<WebCore::PlatformSpeechSynthesisUtterance>&&)utterance;
- (void)pause;
- (void)resume;
- (void)cancel;
- (void)resetState;

@end

@implementation WebSpeechSynthesisWrapper

- (WebSpeechSynthesisWrapper *)initWithSpeechSynthesizer:(WebCore::PlatformSpeechSynthesizer*)synthesizer
{
    if (!(self = [super init]))
        return nil;

    m_synthesizerObject = synthesizer;

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(availableVoicesDidChange) name:avSpeechSynthesisAvailableVoicesDidChangeNotification() object:nil];

    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self name:avSpeechSynthesisAvailableVoicesDidChangeNotification() object:nil];
    [super dealloc];
}

- (void)availableVoicesDidChange
{
    // AVFoundation may post this from a background thread, while the synthesizer and its WeakPtr are
    // main-thread objects.
    ensureOnMainThread([retainedSelf = retainPtr(self)] {
        if (RefPtr synthesizer = retainedSelf->m_synthesizerObject.get())
            synthesizer->voicesDidChange();
    });
}

- (float)mapSpeechRateToPlatformRate:(float)rate
{
    // WebSpeech says to go from .1 -> 10 (default 1)
    // AVSpeechSynthesizer asks for 0 -> 1 (default .5)
    if (rate < 1)
        rate *= avSpeechUtteranceDefaultSpeechRate();
    else
        rate = avSpeechUtteranceDefaultSpeechRate() + ((rate - 1) * (avSpeechUtteranceMaximumSpeechRate() - avSpeechUtteranceDefaultSpeechRate()));

    return rate;
}

- (bool)isAudioOutputEnabled
{
    RefPtr synthesizer = m_synthesizerObject.get();
    if (!synthesizer)
        return true;

    if (RefPtr client = synthesizer->client())
        return client->isAudioOutputEnabled();

    return true;
}

- (void)speakUtterance:(RefPtr<WebCore::PlatformSpeechSynthesisUtterance>&&)utterance
{
    ASSERT(utterance);
    if (!utterance)
        return;

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    if (!m_synthesizer) {
        lazyInitialize(m_synthesizer, adoptNS([AVSpeechSynthesizer new]));
        [m_synthesizer setDelegate:self];
    }

    // Choose the best voice, by first looking at the utterance voice, then the utterance language,
    // then choose the default language.
    RefPtr utteranceVoice = utterance->voice();
    RetainPtr<NSString> voiceLanguage;
    if (!utteranceVoice || utteranceVoice->voiceURI().isEmpty()) {
        if (utterance->lang().isEmpty())
            voiceLanguage = [AVSpeechSynthesisVoice currentLanguageCode];
        else
            voiceLanguage = utterance->lang().createNSString();
    } else
        voiceLanguage = utterance->voice()->lang().createNSString();

    AVSpeechSynthesisVoice *avVoice = nil;
    if (utteranceVoice)
        avVoice = [AVSpeechSynthesisVoice voiceWithIdentifier:utteranceVoice->voiceURI().createNSString().get()];

    if (!avVoice)
        avVoice = [AVSpeechSynthesisVoice voiceWithLanguage:voiceLanguage.get()];

    RetainPtr<AVSpeechUtterance> avUtterance = adoptNS([[AVSpeechUtterance alloc] initWithString:utterance->text().createNSString().get()]);

    [avUtterance setRate:[self mapSpeechRateToPlatformRate:utterance->rate()]];
    // With audio output off nothing may be heard: the utterance still runs, at its real pace, into a
    // sink that drops the samples.
    [avUtterance setVolume:([self isAudioOutputEnabled] ? utterance->volume() : 0)];
    [avUtterance setPitchMultiplier:utterance->pitch()];
    [avUtterance setVoice:avVoice];
    m_avUtterance = avUtterance;
    m_utterance = WTF::move(utterance);

    [m_synthesizer speakUtterance:avUtterance.get()];
    END_BLOCK_OBJC_EXCEPTIONS
}

- (void)pause
{
    if (!m_utterance)
        return;

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    [m_synthesizer pauseSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    END_BLOCK_OBJC_EXCEPTIONS
}

- (void)resume
{
    if (!m_utterance)
        return;

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    [m_synthesizer continueSpeaking];
    END_BLOCK_OBJC_EXCEPTIONS
}

- (void)resetState
{
    // On a reset, cancel utterance and forget it immediately so the next speech job continues without waiting for a callback.
    [self cancel];
    m_utterance = nullptr;
    m_avUtterance = nullptr;
}

- (void)cancel
{
    if (!m_utterance)
        return;

    BEGIN_BLOCK_OBJC_EXCEPTIONS
    [m_synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    END_BLOCK_OBJC_EXCEPTIONS
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer didStartSpeechUtterance:(AVSpeechUtterance *)utterance
{
    UNUSED_PARAM(synthesizer);
    if (!m_utterance || m_avUtterance.get() != utterance)
        return;

    RefPtr protectedSynthesizerObject = m_synthesizerObject.get();
    if (!protectedSynthesizerObject)
        return;

    if (RefPtr client = protectedSynthesizerObject->client())
        client->didStartSpeaking(Ref { *m_utterance });
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer didFinishSpeechUtterance:(AVSpeechUtterance *)utterance
{
    UNUSED_PARAM(synthesizer);
    if (!m_utterance || m_avUtterance.get() != utterance)
        return;

    RefPtr protectedSynthesizerObject = m_synthesizerObject.get();
    if (!protectedSynthesizerObject)
        return;

    // Clear the utterance in case finishing speaking kicks off a new speaking job immediately.
    RefPtr<WebCore::PlatformSpeechSynthesisUtterance> protectedUtterance = m_utterance;
    m_utterance = nullptr;
    m_avUtterance = nullptr;

    if (RefPtr client = protectedSynthesizerObject->client())
        client->didFinishSpeaking(*protectedUtterance);
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer didPauseSpeechUtterance:(AVSpeechUtterance *)utterance
{
    UNUSED_PARAM(synthesizer);
    if (!m_utterance || m_avUtterance.get() != utterance)
        return;

    RefPtr protectedSynthesizerObject = m_synthesizerObject.get();
    if (!protectedSynthesizerObject)
        return;

    if (RefPtr client = protectedSynthesizerObject->client())
        client->didPauseSpeaking(Ref { *m_utterance });
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer didContinueSpeechUtterance:(AVSpeechUtterance *)utterance
{
    UNUSED_PARAM(synthesizer);
    if (!m_utterance || m_avUtterance.get() != utterance)
        return;

    RefPtr protectedSynthesizerObject = m_synthesizerObject.get();
    if (!protectedSynthesizerObject)
        return;

    if (RefPtr client = protectedSynthesizerObject->client())
        client->didResumeSpeaking(Ref { *m_utterance });
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer didCancelSpeechUtterance:(AVSpeechUtterance *)utterance
{
    UNUSED_PARAM(synthesizer);
    if (!m_utterance || m_avUtterance.get() != utterance)
        return;

    RefPtr protectedSynthesizerObject = m_synthesizerObject.get();
    if (!protectedSynthesizerObject)
        return;

    // Clear the utterance in case cancelling kicks off a new speaking job immediately.
    RefPtr<WebCore::PlatformSpeechSynthesisUtterance> protectedUtterance = m_utterance;
    m_utterance = nullptr;
    m_avUtterance = nullptr;

    if (RefPtr client = protectedSynthesizerObject->client())
        client->didFinishSpeaking(*protectedUtterance);
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer willSpeakRangeOfSpeechString:(NSRange)characterRange utterance:(AVSpeechUtterance *)utterance
{
    UNUSED_PARAM(synthesizer);
    if (!m_utterance || m_avUtterance.get() != utterance)
        return;

    RefPtr protectedSynthesizerObject = m_synthesizerObject.get();
    if (!protectedSynthesizerObject)
        return;

    // AVSpeechSynthesizer only supports word boundaries.
    if (RefPtr client = protectedSynthesizerObject->client())
        client->boundaryEventOccurred(Ref { *m_utterance }, WebCore::SpeechBoundary::SpeechWordBoundary, characterRange.location, characterRange.length);
}

@end

namespace WebCore {

Ref<PlatformSpeechSynthesizer> PlatformSpeechSynthesizer::create(PlatformSpeechSynthesizerClient& client)
{
    return adoptRef(*new PlatformSpeechSynthesizer(client));
}

PlatformSpeechSynthesizer::PlatformSpeechSynthesizer(PlatformSpeechSynthesizerClient& client)
    : m_speechSynthesizerClient(client)
{
}

PlatformSpeechSynthesizer::~PlatformSpeechSynthesizer() = default;

void PlatformSpeechSynthesizer::appendVoices(NSArray *voices)
{
    for (SUPPRESS_UNRETAINED_LOCAL AVSpeechSynthesisVoice *voice in voices) {
        if (voice.isSystemVoice)
            m_voiceList.append(PlatformSpeechSynthesisVoice::create(voice.identifier, voice.name, voice.language, /* localService */ true, /* isDefault */ true));
    }
}

void PlatformSpeechSynthesizer::initializeVoiceList()
{
    BEGIN_BLOCK_OBJC_EXCEPTIONS

    Class avSpeechSynthesisVoiceClass = [AVSpeechSynthesisVoice class];

    // Support older OS versions that don't have the asynchronous version yet.
    if (![avSpeechSynthesisVoiceClass respondsToSelector:@selector(speechVoicesIncludingSuperCompactWithCompletionHandler:)]) {
        appendVoices([avSpeechSynthesisVoiceClass speechVoicesIncludingSuperCompact]);
        return;
    }

    WeakPtr weakThis { *this };
    [avSpeechSynthesisVoiceClass speechVoicesIncludingSuperCompactWithCompletionHandler:^(NSArray<AVSpeechSynthesisVoice *> *voices) {
        callOnMainThread([weakThis, voices = RetainPtr { voices }]() {
            BEGIN_BLOCK_OBJC_EXCEPTIONS
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;

            protectedThis->appendVoices(voices.get());
            if (RefPtr client = protectedThis->client())
                client->voicesDidChange();
            END_BLOCK_OBJC_EXCEPTIONS
        });
    }];

    END_BLOCK_OBJC_EXCEPTIONS
}

void PlatformSpeechSynthesizer::pause()
{
    [m_platformSpeechWrapper pause];
}

void PlatformSpeechSynthesizer::resume()
{
    [m_platformSpeechWrapper resume];
}

void PlatformSpeechSynthesizer::speak(RefPtr<PlatformSpeechSynthesisUtterance>&& utterance)
{
    if (!m_platformSpeechWrapper)
        lazyInitialize(m_platformSpeechWrapper, adoptNS([[WebSpeechSynthesisWrapper alloc] initWithSpeechSynthesizer:this]));

    [m_platformSpeechWrapper speakUtterance:utterance.get()];
}

void PlatformSpeechSynthesizer::cancel()
{
    [m_platformSpeechWrapper cancel];
}

void PlatformSpeechSynthesizer::resetState()
{
    [m_platformSpeechWrapper resetState];
}

} // namespace WebCore

#endif // ENABLE(SPEECH_SYNTHESIS) && PLATFORM(WPE) && OS(DARWIN)
