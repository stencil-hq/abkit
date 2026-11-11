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

#if USE(SKIA)

#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// Big-endian reads from a font table that fail instead of running past its end.
class FontTableReader {
public:
    explicit FontTableReader(const SkData& data)
        : m_data(unsafeMakeSpan(data.bytes(), data.size()))
    {
    }

    std::optional<uint32_t> unsignedAt(size_t offset, size_t size) const
    {
        if (offset > m_data.size() || m_data.size() - offset < size)
            return std::nullopt;
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i)
            value = value << 8 | m_data[offset + i];
        return value;
    }

    // Signed integers of one, two or four bytes.
    std::optional<int32_t> signedAt(size_t offset, size_t size) const
    {
        auto value = unsignedAt(offset, size);
        if (!value)
            return std::nullopt;
        switch (size) {
        case 1:
            return static_cast<int8_t>(*value);
        case 2:
            return static_cast<int16_t>(*value);
        default:
            return static_cast<int32_t>(*value);
        }
    }

    std::optional<uint16_t> uint16At(size_t offset) const
    {
        if (auto value = unsignedAt(offset, 2))
            return static_cast<uint16_t>(*value);
        return std::nullopt;
    }

    std::optional<int16_t> int16At(size_t offset) const
    {
        if (auto value = unsignedAt(offset, 2))
            return static_cast<int16_t>(*value);
        return std::nullopt;
    }

    std::optional<uint32_t> uint32At(size_t offset) const { return unsignedAt(offset, 4); }

    // A 16.16 fixed point number.
    std::optional<float> fixedAt(size_t offset) const
    {
        if (auto value = unsignedAt(offset, 4))
            return static_cast<int32_t>(*value) / 65536.f;
        return std::nullopt;
    }

private:
    std::span<const uint8_t> m_data;
};

} // namespace WebCore

#endif // USE(SKIA)
