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
#include "FontTrackingSkia.h"

#if USE(SKIA)

#include "FontTableReaderSkia.h"
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkTypeface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FontTracking);

FontTracking::FontTracking(Vector<Stop>&& stops)
    : m_stops(WTF::move(stops))
{
}

std::unique_ptr<FontTracking> FontTracking::create(const SkTypeface& typeface)
{
    static constexpr SkFontTableTag trackingTag = SkSetFourByteTag('t', 'r', 'a', 'k');
    auto table = typeface.copyTableData(trackingTag);
    if (!table)
        return nullptr;
    FontTableReader reader(*table);

    // The header: version (fixed), format, offset of the horizontal data, offset of the vertical data.
    auto horizontalOffset = reader.uint16At(6);
    if (!horizontalOffset || !*horizontalOffset)
        return nullptr;

    // The data: the number of tracks and of sizes, the offset of the sizes, then one entry for
    // each track: its value (fixed), the name of the track, the offset of its per-size values.
    auto trackCount = reader.uint16At(*horizontalOffset);
    auto sizeCount = reader.uint16At(*horizontalOffset + 2);
    auto sizesOffset = reader.uint32At(*horizontalOffset + 4);
    if (!trackCount || !sizeCount || !sizesOffset || !*trackCount || !*sizeCount)
        return nullptr;

    struct Track {
        float value;
        uint16_t offset;
    };
    Vector<Track> tracks;
    for (unsigned i = 0; i < *trackCount; ++i) {
        size_t entry = *horizontalOffset + 8 + i * 8;
        auto value = reader.fixedAt(entry);
        auto offset = reader.uint16At(entry + 6);
        if (!value || !offset)
            return nullptr;
        tracks.append({ *value, *offset });
    }

    // Text is set on the normal track, number zero. A font with a single track has nothing else to
    // offer; a font without a normal track gets one between its nearest looser and tighter tracks.
    size_t below = 0;
    size_t above = tracks.size() - 1;
    while (below + 1 < tracks.size() && tracks[below + 1].value <= 0)
        ++below;
    while (above > 0 && tracks[above - 1].value >= 0)
        --above;
    float span = tracks[above].value - tracks[below].value;
    float weight = below == above || !span ? 0 : -tracks[below].value / span;

    Vector<Stop> stops;
    for (unsigned i = 0; i < *sizeCount; ++i) {
        auto size = reader.fixedAt(*sizesOffset + i * 4);
        auto valueBelow = reader.int16At(tracks[below].offset + i * 2);
        auto valueAbove = reader.int16At(tracks[above].offset + i * 2);
        if (!size || !valueBelow || !valueAbove)
            return nullptr;
        stops.append({ *size, *valueBelow + weight * (*valueAbove - *valueBelow) });
    }
    return std::unique_ptr<FontTracking>(new FontTracking(WTF::move(stops)));
}

float FontTracking::valueForSize(float size) const
{
    auto interpolated = [&] {
        if (size <= m_stops.first().size)
            return m_stops.first().value;
        for (size_t i = 1; i < m_stops.size(); ++i) {
            auto& before = m_stops[i - 1];
            auto& after = m_stops[i];
            if (size > after.size)
                continue;
            if (after.size == before.size)
                return after.value;
            return before.value + (after.value - before.value) * (size - before.size) / (after.size - before.size);
        }
        return m_stops.last().value;
    }();
    // Core Text works in whole design units and drops the fraction.
    return std::trunc(interpolated);
}

} // namespace WebCore

#endif // USE(SKIA)
