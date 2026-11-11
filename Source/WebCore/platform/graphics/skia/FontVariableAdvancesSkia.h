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

#include <memory>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkTypeface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

// The advances of an instance of a variable font, from its 'hmtx' and 'HVAR' tables. FreeType keeps
// the advance of an instance in whole design units; Core Text keeps the fraction that interpolating
// between the font's masters leaves, and text set in an instance (the bold of the Mac's system font,
// say) is only as wide as in the other browsers of a Mac when that fraction is kept here too.
class FontVariableAdvances {
    WTF_MAKE_TZONE_ALLOCATED(FontVariableAdvances);
public:
    // Null when the typeface is not an instance away from the default of a font with these tables.
    static std::unique_ptr<FontVariableAdvances> create(const SkTypeface&);

    // In font design units.
    float advance(SkGlyphID) const;

private:
    FontVariableAdvances() = default;

    struct ItemData {
        size_t offset { 0 };
        unsigned itemCount { 0 };
        unsigned wordCount { 0 };
        bool hasLongWords { false };
        // The share of each of the item's regions at the position of the instance.
        Vector<float> scalars;
    };

    float delta(unsigned outerIndex, unsigned innerIndex) const;

    sk_sp<SkData> m_metrics;
    sk_sp<SkData> m_variations;
    unsigned m_metricCount { 0 };
    Vector<ItemData> m_items;
    // The map from glyphs to delta sets, when the font has one.
    size_t m_mapOffset { 0 };
    unsigned m_mapCount { 0 };
    unsigned m_mapEntrySize { 0 };
    unsigned m_mapInnerBits { 0 };
};

} // namespace WebCore

#endif // USE(SKIA)
