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

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && PLATFORM(WPE) && OS(DARWIN) && !ENABLE(THUNDER)

#include "CDMProxy.h"
#include "GStreamerEMEUtilities.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

// The factory that gives every ClearKey CDM instance the proxy its decryptor reads keys from.
class CDMProxyFactoryClearKey final : public CDMProxyFactory {
    WTF_MAKE_TZONE_ALLOCATED(CDMProxyFactoryClearKey);
public:
    static CDMProxyFactoryClearKey& singleton();

    ~CDMProxyFactoryClearKey() = default;

private:
    friend class NeverDestroyed<CDMProxyFactoryClearKey>;

    CDMProxyFactoryClearKey() = default;
    RefPtr<CDMProxy> createCDMProxy(const String&) final;
    bool supportsKeySystem(const String&) final;
};

// The ClearKey CDM instance proxy: it decrypts CENC ClearKey samples with AES-128-CTR, off the main
// thread (the decryptor element calls it from the streaming thread).
class CDMProxyClearKey final : public CDMProxy, public CanMakeWeakPtr<CDMProxyClearKey, WeakPtrFactoryInitialization::Eager> {
public:
    CDMProxyClearKey() = default;
    virtual ~CDMProxyClearKey() = default;

    struct DecryptionContext {
        GstBuffer* keyIDBuffer;
        GstBuffer* ivBuffer;
        GstBuffer* dataBuffer;
        GstBuffer* subsamplesBuffer;
        size_t numSubsamples;
        WeakPtr<CDMProxyDecryptionClient> cdmProxyDecryptionClient;
    };

    // Decrypts the encrypted bytes of one sample in place, waiting for its key when it is not in the
    // proxy's key store yet.
    DecryptionResult decrypt(DecryptionContext&);
};

} // namespace WebCore

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && PLATFORM(WPE) && OS(DARWIN) && !ENABLE(THUNDER)
