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
#include "FontVariableAdvancesSkia.h"

#if USE(SKIA)

#include "FontTableReaderSkia.h"
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FontVariableAdvances);

static constexpr int normalizedOne = 1 << 14;

// The position of the instance on each axis, from -1 at the minimum of the axis over 0 at its
// default to 1 at its maximum, as a 16.16 fixed point number, cut off as Core Text cuts it off.
// The font's tables count in 2.14 numbers; Core Text bends the axes ('avar') in 16.16, rounding
// down, and only then rounds to 2.14. The last digits of its advances come from that order.
static Vector<int> normalizedCoordinates(const SkTypeface& typeface)
{
    int axisCount = typeface.getVariationDesignParameters({ });
    if (axisCount <= 0)
        return { };
    Vector<SkFontParameters::Variation::Axis> axes(axisCount);
    if (typeface.getVariationDesignParameters(axes.mutableSpan()) != axisCount)
        return { };

    Vector<SkFontArguments::VariationPosition::Coordinate> position;
    if (int count = typeface.getVariationDesignPosition({ }); count > 0) {
        position.grow(count);
        if (typeface.getVariationDesignPosition(position.mutableSpan()) != count)
            return { };
    }

    return axes.map([&](auto& axis) {
        float value = axis.def;
        for (auto& coordinate : position) {
            if (coordinate.axis == axis.tag)
                value = std::clamp(coordinate.value, axis.min, axis.max);
        }
        double normalized = 0;
        if (value < axis.def)
            normalized = (static_cast<double>(value) - axis.def) / (static_cast<double>(axis.def) - axis.min);
        else if (value > axis.def)
            normalized = (static_cast<double>(value) - axis.def) / (static_cast<double>(axis.max) - axis.def);
        return static_cast<int>(normalized * 65536);
    });
}

// The 'avar' table bends each axis: a list of points, per axis, between which the normalized
// coordinate is interpolated. False for a table this code cannot follow.
static bool applyAxisVariations(const SkTypeface& typeface, Vector<int>& coordinates)
{
    static constexpr SkFontTableTag axisVariationsTag = SkSetFourByteTag('a', 'v', 'a', 'r');
    auto table = typeface.copyTableData(axisVariationsTag);
    if (!table)
        return true;
    FontTableReader reader(*table);
    auto majorVersion = reader.uint16At(0);
    auto axisCount = reader.uint16At(6);
    if (!majorVersion || *majorVersion != 1 || !axisCount || *axisCount != coordinates.size())
        return false;

    size_t offset = 8;
    for (auto& coordinate : coordinates) {
        auto pointCount = reader.uint16At(offset);
        if (!pointCount)
            return false;
        offset += 2;
        int previousFrom = 0;
        int previousTo = 0;
        bool mapped = false;
        for (unsigned i = 0; i < *pointCount; ++i) {
            auto from = reader.int16At(offset + i * 4);
            auto to = reader.int16At(offset + i * 4 + 2);
            if (!from || !to)
                return false;
            if (mapped)
                continue;
            // The points are 2.14 numbers, the coordinate a 16.16 one.
            int currentFrom = *from * 4;
            int currentTo = *to * 4;
            if (coordinate == currentFrom) {
                coordinate = currentTo;
                mapped = true;
            } else if (i && coordinate < currentFrom) {
                int64_t numerator = static_cast<int64_t>(coordinate - previousFrom) * (currentTo - previousTo);
                int64_t denominator = currentFrom - previousFrom;
                int64_t quotient = numerator / denominator;
                if (numerator % denominator && numerator < 0)
                    --quotient;
                coordinate = previousTo + static_cast<int>(quotient);
                mapped = true;
            }
            previousFrom = currentFrom;
            previousTo = currentTo;
        }
        offset += *pointCount * 4;
    }
    return true;
}

std::unique_ptr<FontVariableAdvances> FontVariableAdvances::create(const SkTypeface& typeface)
{
    auto coordinates = normalizedCoordinates(typeface);
    if (std::ranges::all_of(coordinates, [](int coordinate) { return !coordinate; }))
        return nullptr;
    if (!applyAxisVariations(typeface, coordinates))
        return nullptr;
    for (auto& coordinate : coordinates)
        coordinate = std::clamp((coordinate + 2) >> 2, -normalizedOne, normalizedOne);

    static constexpr SkFontTableTag metricsTag = SkSetFourByteTag('h', 'm', 't', 'x');
    static constexpr SkFontTableTag headerTag = SkSetFourByteTag('h', 'h', 'e', 'a');
    static constexpr SkFontTableTag variationsTag = SkSetFourByteTag('H', 'V', 'A', 'R');
    auto result = std::unique_ptr<FontVariableAdvances>(new FontVariableAdvances);
    result->m_metrics = typeface.copyTableData(metricsTag);
    result->m_variations = typeface.copyTableData(variationsTag);
    auto header = typeface.copyTableData(headerTag);
    if (!result->m_metrics || !result->m_variations || !header)
        return nullptr;

    auto metricCount = FontTableReader(*header).uint16At(34);
    if (!metricCount || !*metricCount || result->m_metrics->size() < *metricCount * 4u)
        return nullptr;
    result->m_metricCount = *metricCount;

    // The table: its version, the offset of the store of deltas, the offset of the map from glyphs
    // to delta sets. The store: its format, the offset of its regions, then its items by offset.
    FontTableReader reader(*result->m_variations);
    auto majorVersion = reader.uint16At(0);
    auto storeOffset = reader.uint32At(4);
    auto mapOffset = reader.uint32At(8);
    if (!majorVersion || *majorVersion != 1 || !storeOffset || !*storeOffset || !mapOffset)
        return nullptr;
    auto storeFormat = reader.uint16At(*storeOffset);
    auto regionsOffset = reader.uint32At(*storeOffset + 2);
    auto itemCount = reader.uint16At(*storeOffset + 6);
    if (!storeFormat || *storeFormat != 1 || !regionsOffset || !itemCount)
        return nullptr;

    // A region is a tent over each axis: zero outside its start and end, one at its peak. What it
    // contributes at the position of the instance is the product of its tents there.
    size_t regions = *storeOffset + *regionsOffset;
    auto regionAxisCount = reader.uint16At(regions);
    auto regionCount = reader.uint16At(regions + 2);
    if (!regionAxisCount || !regionCount || *regionAxisCount != coordinates.size())
        return nullptr;
    Vector<float> regionScalars;
    for (unsigned region = 0; region < *regionCount; ++region) {
        float scalar = 1;
        for (unsigned axis = 0; axis < *regionAxisCount; ++axis) {
            size_t tent = regions + 4 + (region * *regionAxisCount + axis) * 6;
            auto start = reader.int16At(tent);
            auto peak = reader.int16At(tent + 2);
            auto end = reader.int16At(tent + 4);
            if (!start || !peak || !end)
                return nullptr;
            int coordinate = coordinates[axis];
            if (!*peak || *start > *peak || *peak > *end || (*start < 0 && *end > 0))
                continue;
            if (coordinate == *peak)
                continue;
            if (coordinate <= *start || coordinate >= *end) {
                scalar = 0;
                break;
            }
            if (coordinate < *peak)
                scalar *= static_cast<float>(coordinate - *start) / (*peak - *start);
            else
                scalar *= static_cast<float>(*end - coordinate) / (*end - *peak);
        }
        regionScalars.append(scalar);
    }

    // An item: the number of its delta sets, how many of the deltas of a set are wide, the regions
    // the deltas of a set belong to, then the sets.
    for (unsigned i = 0; i < *itemCount; ++i) {
        auto itemOffset = reader.uint32At(*storeOffset + 8 + i * 4);
        if (!itemOffset)
            return nullptr;
        size_t item = *storeOffset + *itemOffset;
        auto setCount = reader.uint16At(item);
        auto wordCount = reader.uint16At(item + 2);
        auto itemRegionCount = reader.uint16At(item + 4);
        if (!setCount || !wordCount || !itemRegionCount)
            return nullptr;
        ItemData data;
        data.itemCount = *setCount;
        data.hasLongWords = *wordCount & 0x8000;
        data.wordCount = *wordCount & 0x7fff;
        if (data.wordCount > *itemRegionCount)
            return nullptr;
        for (unsigned region = 0; region < *itemRegionCount; ++region) {
            auto regionIndex = reader.uint16At(item + 6 + region * 2);
            if (!regionIndex || *regionIndex >= regionScalars.size())
                return nullptr;
            data.scalars.append(regionScalars[*regionIndex]);
        }
        data.offset = item + 6 + *itemRegionCount * 2;
        result->m_items.append(WTF::move(data));
    }

    // The map: its format, the layout of its entries, their number, the entries. A font without a
    // map has one delta set for each glyph, in its first item.
    if (*mapOffset) {
        auto mapFormat = reader.unsignedAt(*mapOffset, 1);
        auto entryFormat = reader.unsignedAt(*mapOffset + 1, 1);
        if (!mapFormat || *mapFormat > 1 || !entryFormat)
            return nullptr;
        auto mapCount = *mapFormat ? reader.uint32At(*mapOffset + 2) : reader.unsignedAt(*mapOffset + 2, 2);
        if (!mapCount)
            return nullptr;
        result->m_mapOffset = *mapOffset + (*mapFormat ? 6 : 4);
        result->m_mapCount = *mapCount;
        result->m_mapEntrySize = ((*entryFormat & 0x30) >> 4) + 1;
        result->m_mapInnerBits = (*entryFormat & 0x0f) + 1;
    }
    return result;
}

float FontVariableAdvances::delta(unsigned outerIndex, unsigned innerIndex) const
{
    if (outerIndex >= m_items.size())
        return 0;
    auto& item = m_items[outerIndex];
    if (innerIndex >= item.itemCount)
        return 0;

    unsigned wordSize = item.hasLongWords ? 4 : 2;
    unsigned shortSize = item.hasLongWords ? 2 : 1;
    size_t setSize = item.wordCount * wordSize + (item.scalars.size() - item.wordCount) * shortSize;
    size_t offset = item.offset + innerIndex * setSize;
    FontTableReader reader(*m_variations);
    float result = 0;
    for (unsigned region = 0; region < item.scalars.size(); ++region) {
        unsigned size = region < item.wordCount ? wordSize : shortSize;
        auto value = reader.signedAt(offset, size);
        if (!value)
            return 0;
        result += item.scalars[region] * *value;
        offset += size;
    }
    return result;
}

float FontVariableAdvances::advance(SkGlyphID glyph) const
{
    // Glyphs past the last metric share its advance.
    auto base = FontTableReader(*m_metrics).uint16At(std::min<unsigned>(glyph, m_metricCount - 1) * 4);
    if (!base)
        return 0;

    unsigned outerIndex = 0;
    unsigned innerIndex = glyph;
    if (m_mapCount) {
        // Glyphs past the last entry share it.
        auto entry = FontTableReader(*m_variations).unsignedAt(m_mapOffset + std::min<unsigned>(glyph, m_mapCount - 1) * m_mapEntrySize, m_mapEntrySize);
        if (!entry)
            return *base;
        outerIndex = *entry >> m_mapInnerBits;
        innerIndex = *entry & ((1u << m_mapInnerBits) - 1);
    }
    return *base + delta(outerIndex, innerIndex);
}

} // namespace WebCore

#endif // USE(SKIA)
