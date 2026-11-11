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

class SkTypeface;

namespace WebCore {

// The horizontal 'trak' table of a font: what its designers add to every advance, as a function of
// the size of the text. Core Text applies it by itself to fonts that also have a 'STAT' table, which
// is how the Mac's interface fonts get wider spacing in small text and tighter spacing in large text.
class FontTracking {
    WTF_MAKE_TZONE_ALLOCATED(FontTracking);
public:
    // Null when the typeface has no usable table.
    static std::unique_ptr<FontTracking> create(const SkTypeface&);

    // In font design units, for text of the given size.
    float valueForSize(float) const;

private:
    struct Stop {
        float size;
        float value;
    };
    explicit FontTracking(Vector<Stop>&&);

    Vector<Stop> m_stops;
};

} // namespace WebCore

#endif // USE(SKIA)
