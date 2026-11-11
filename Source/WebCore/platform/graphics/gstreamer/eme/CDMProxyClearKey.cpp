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
#include "CDMProxyClearKey.h"

#if ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && PLATFORM(WPE) && OS(DARWIN) && !ENABLE(THUNDER)

#include "GStreamerCommon.h"
#include <CommonCrypto/CommonCrypto.h>
#include <array>
#include <optional>
#include <string.h>
#include <utility>
#include <wtf/Scope.h>
#include <wtf/TZoneMallocInlines.h>

GST_DEBUG_CATEGORY_EXTERN(webkitMediaClearKeyDecryptDebugCategory);
#define GST_CAT_DEFAULT webkitMediaClearKeyDecryptDebugCategory

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CDMProxyFactoryClearKey);

namespace {

// A CENC subsample table entry is the number of clear bytes of the subsample, then the number of
// encrypted bytes (ISO/IEC 23001-7:2016 section 9.6), both big endian.
static bool readBigEndianUInt16(const uint8_t* buffer, size_t bufferSize, size_t& offset, uint16_t& value)
{
    ASSERT_ARG(offset, offset <= bufferSize);
    if (bufferSize - offset < sizeof(value))
        return false;

    value = (static_cast<uint16_t>(buffer[offset]) << 8) | static_cast<uint16_t>(buffer[offset + 1]);
    offset += sizeof(value);
    return true;
}

static bool readBigEndianUInt32(const uint8_t* buffer, size_t bufferSize, size_t& offset, uint32_t& value)
{
    ASSERT_ARG(offset, offset <= bufferSize);
    if (bufferSize - offset < sizeof(value))
        return false;

    value = (static_cast<uint32_t>(buffer[offset]) << 24) | (static_cast<uint32_t>(buffer[offset + 1]) << 16)
        | (static_cast<uint32_t>(buffer[offset + 2]) << 8) | static_cast<uint32_t>(buffer[offset + 3]);
    offset += sizeof(value);
    return true;
}

// The AES-CTR counter of a sample: the least significant 8 bytes of a 16 byte counter are zero when
// the sample carries an 8 byte IV (ISO/IEC 23001-7:2016 section 9.3).
static std::optional<std::array<uint8_t, kCCBlockSizeAES128>> makeCounter(const uint8_t* iv, size_t ivSize)
{
    if (ivSize != 8 && ivSize < kCCBlockSizeAES128)
        return std::nullopt;

    std::array<uint8_t, kCCBlockSizeAES128> counter { 0 };
    memcpy(counter.data(), iv, ivSize == 8 ? 8 : counter.size());
    return counter;
}

// AES-128-CTR over the encrypted bytes of one sample, in place. ClearKey has no keys smaller or
// larger than AES-128, and CTR needs the encrypt operation for both directions.
static bool decryptAES128CTR(const Vector<uint8_t>& key, const std::array<uint8_t, kCCBlockSizeAES128>& counter, uint8_t* data, size_t size)
{
    CCCryptorRef cryptor = nullptr;
    if (CCCryptorCreateWithMode(kCCEncrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, counter.data(), key.data(), key.size(), nullptr, 0, 0, kCCModeOptionCTR_BE, &cryptor) != kCCSuccess) {
        GST_ERROR("CommonCrypto could not start an AES-128-CTR cryptor");
        return false;
    }

    auto releaseCryptor = makeScopeExit([&] {
        CCCryptorRelease(cryptor);
    });

    // The output buffer must not overlap the input one, so the plaintext is written aside.
    Vector<uint8_t> plainText(CCCryptorGetOutputLength(cryptor, size, true));
    size_t bytesWritten = 0;
    if (CCCryptorUpdate(cryptor, data, size, plainText.data(), plainText.size(), &bytesWritten) != kCCSuccess) {
        GST_ERROR("CommonCrypto AES-128-CTR update failed");
        return false;
    }

    size_t finalBytesWritten = 0;
    if (CCCryptorFinal(cryptor, plainText.data() + bytesWritten, plainText.size() - bytesWritten, &finalBytesWritten) != kCCSuccess) {
        GST_ERROR("CommonCrypto AES-128-CTR finalization failed");
        return false;
    }

    if (bytesWritten + finalBytesWritten != size) {
        GST_ERROR("CommonCrypto AES-128-CTR produced %zu bytes of %zu", bytesWritten + finalBytesWritten, size);
        return false;
    }

    memcpy(data, plainText.data(), size);
    return true;
}

} // namespace

CDMProxyFactoryClearKey& CDMProxyFactoryClearKey::singleton()
{
    static NeverDestroyed<CDMProxyFactoryClearKey> s_factory;
    return s_factory;
}

RefPtr<CDMProxy> CDMProxyFactoryClearKey::createCDMProxy(const String& keySystem)
{
    ASSERT_UNUSED(keySystem, supportsKeySystem(keySystem));
    return adoptRef(new CDMProxyClearKey());
}

bool CDMProxyFactoryClearKey::supportsKeySystem(const String& keySystem)
{
    // `org.w3.clearkey` is the only supported key system.
    return equalLettersIgnoringASCIICase(keySystem, "org.w3.clearkey"_s);
}

DecryptionResult CDMProxyClearKey::decrypt(CDMProxyClearKey::DecryptionContext& input)
{
    GstMappedBuffer mappedKeyID(input.keyIDBuffer, GST_MAP_READ);
    if (!mappedKeyID) {
        GST_ERROR("Failed to map key ID buffer");
        return DecryptionResult::Failure;
    }

    GstMappedBuffer mappedIV(input.ivBuffer, GST_MAP_READ);
    if (!mappedIV) {
        GST_ERROR("Failed to map IV buffer");
        return DecryptionResult::Failure;
    }

    GstMappedBuffer mappedData(input.dataBuffer, GST_MAP_READWRITE);
    if (!mappedData) {
        GST_ERROR("Failed to map data buffer");
        return DecryptionResult::Failure;
    }

    auto iv = mappedIV.createVector();
    auto counter = makeCounter(iv.data(), iv.size());
    if (!counter) {
        GST_ERROR("ClearKey IV of %zu bytes is neither 8 nor 16 bytes", iv.size());
        return DecryptionResult::Failure;
    }

    auto keyData = getOrWaitForKeyValue(mappedKeyID.createVector(), WTF::move(input.cdmProxyDecryptionClient));
    if (!keyData)
        return DecryptionResult::NoKey;

    ASSERT(std::holds_alternative<Vector<uint8_t>>(*keyData));
    auto& key = std::get<Vector<uint8_t>>(*keyData);
    if (key.size() != kCCKeySizeAES128) {
        GST_ERROR("ClearKey key of %zu bytes is not an AES-128 key", key.size());
        return DecryptionResult::Failure;
    }

    // The encrypted ranges of the sample, in the order the AES-CTR counter consumes them.
    Vector<std::pair<size_t, size_t>> encryptedRanges;
    if (!input.numSubsamples) {
        if (mappedData.size())
            encryptedRanges.append(std::pair<size_t, size_t> { 0, mappedData.size() });
    } else {
        GstMappedBuffer mappedSubsamples(input.subsamplesBuffer, GST_MAP_READ);
        if (!mappedSubsamples) {
            GST_ERROR("Failed to map subsamples buffer");
            return DecryptionResult::Failure;
        }

        size_t offsetInData = 0;
        size_t offsetInSubsamples = 0;
        unsigned subsampleIndex = 0;
        while (offsetInData < mappedData.size()) {
            uint16_t numClearBytes = 0;
            uint32_t numEncryptedBytes = 0;
            if (subsampleIndex < input.numSubsamples) {
                if (!readBigEndianUInt16(mappedSubsamples.data(), mappedSubsamples.size(), offsetInSubsamples, numClearBytes)
                    || !readBigEndianUInt32(mappedSubsamples.data(), mappedSubsamples.size(), offsetInSubsamples, numEncryptedBytes)) {
                    GST_ERROR("Could not read subsample %u of the sample", subsampleIndex);
                    return DecryptionResult::Failure;
                }
                subsampleIndex++;
            } else
                numEncryptedBytes = mappedData.size() - offsetInData;

            offsetInData += numClearBytes;
            if (!numEncryptedBytes)
                continue;

            if (numEncryptedBytes > mappedData.size() - offsetInData) {
                GST_ERROR("Subsample %u is larger than the sample", subsampleIndex);
                return DecryptionResult::Failure;
            }

            encryptedRanges.append(std::pair<size_t, size_t> { offsetInData, numEncryptedBytes });
            offsetInData += numEncryptedBytes;
        }
    }

    size_t totalEncryptedBytes = 0;
    for (auto& range : encryptedRanges)
        totalEncryptedBytes += range.second;

    if (!totalEncryptedBytes)
        return DecryptionResult::Success;

    // The counter advances over the encrypted bytes only, so the encrypted ranges are decrypted as
    // one stream, in place.
    Vector<uint8_t> encryptedData(totalEncryptedBytes);
    size_t offset = 0;
    for (auto& range : encryptedRanges) {
        memcpy(encryptedData.data() + offset, mappedData.data() + range.first, range.second);
        offset += range.second;
    }

    if (!decryptAES128CTR(key, *counter, encryptedData.data(), encryptedData.size()))
        return DecryptionResult::Failure;

    offset = 0;
    for (auto& range : encryptedRanges) {
        memcpy(mappedData.data() + range.first, encryptedData.data() + offset, range.second);
        offset += range.second;
    }

    return DecryptionResult::Success;
}

} // namespace WebCore

#undef GST_CAT_DEFAULT

#endif // ENABLE(ENCRYPTED_MEDIA) && USE(GSTREAMER) && PLATFORM(WPE) && OS(DARWIN) && !ENABLE(THUNDER)
