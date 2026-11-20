/*
 * Copyright (C) 2025 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SkiaSystemFallbackFontCache.h"

#if USE(SKIA) && !OS(ANDROID) && !PLATFORM(WIN)
#include "FontCache.h"
#include <fontconfig/fontconfig.h>
#include <wtf/FileSystem.h>
#include <wtf/HashTraits.h>
#include <wtf/Hasher.h>
#include <wtf/text/CharacterProperties.h>

namespace WebCore {

static String filePathFromPattern(FcPattern* pattern)
{
    FcChar8* filename = nullptr;
    if (FcPatternGetString(pattern, FC_FILE, 0, &filename) != FcResultMatch || !filename)
        return { };

    const char* sysroot = reinterpret_cast<const char*>(FcConfigGetSysRoot(nullptr));
    if (!sysroot)
        return String::fromUTF8(reinterpret_cast<const char*>(filename));

    return FileSystem::pathByAppendingComponent(String::fromUTF8(sysroot), String::fromUTF8(reinterpret_cast<const char*>(filename)));
}

// The fonts fontconfig sorts for a locale, best first, to pick the fallback for a character cluster
// from. A sorted set holds every font of the system (thousands on a Mac), and a page in many
// languages makes one per locale, so a font's file is only looked at once the font would be
// picked: the cache fontconfig answers from may name files that are gone.
class FontSetCache {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FontSetCache);
    WTF_MAKE_NONCOPYABLE(FontSetCache);
public:
    struct Font {
        String path;
        int ttcIndex { 0 };
    };

    static std::unique_ptr<FontSetCache> create(const String& locale)
    {
        auto* pattern = FcPatternCreate();
        if (!locale.isNull()) {
            FcLangSet* langSet = FcLangSetCreate();
            FcLangSetAdd(langSet, reinterpret_cast<const FcChar8*>(locale.utf8().legacyCStringPointer()));
            FcPatternAddLangSet(pattern, FC_LANG, langSet);
            FcLangSetDestroy(langSet);
        }

        FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);

        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        FcResult result;
        FcFontSet* fontSet = FcFontSort(nullptr, pattern, FcFalse, nullptr, &result);
        FcPatternDestroy(pattern);

        if (!fontSet)
            return nullptr;

        return makeUnique<FontSetCache>(fontSet);
    }

    explicit FontSetCache(FcFontSet* fontSet)
        : m_fontSet(fontSet)
    {
        m_candidates.reserveInitialCapacity(m_fontSet->nfont);
        for (int i = 0; i < m_fontSet->nfont; ++i) {
            IGNORE_CLANG_WARNINGS_BEGIN("unsafe-buffer-usage")
            FcPattern* pattern = m_fontSet->fonts[i];
            IGNORE_CLANG_WARNINGS_END
            if (!pattern)
                continue;

            FcCharSet* charSet = nullptr;
            if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charSet) != FcResultMatch)
                continue;

            m_candidates.append({ pattern, charSet, { } });
        }
    }

    ~FontSetCache()
    {
        FcFontSetDestroy(m_fontSet);
    }

    // The first font that has every character of the cluster, else the first of those missing the
    // fewest; `fileExists` remembers, across locales, which font files are there.
    const Font* fontForCharacterCluster(FcCharSet* charSet, HashMap<String, bool>& fileExists)
    {
        const Font* bestFont = nullptr;
        int minScore = std::numeric_limits<int>::max();
        for (auto& candidate : m_candidates) {
            int score = FcCharSetSubtractCount(charSet, candidate.charSet);
            if (score >= minScore)
                continue;

            const auto* font = resolve(candidate, fileExists);
            if (!font)
                continue;

            bestFont = font;
            minScore = score;
            if (!score)
                break;
        }

        return bestFont;
    }

private:
    struct Candidate {
        FcPattern* pattern { nullptr }; // Owned by m_fontSet.
        FcCharSet* charSet { nullptr }; // Owned by m_fontSet.
        // Disengaged until first asked for; then the font, or no font when its file is not there.
        std::optional<std::optional<Font>> font;
    };

    static const Font* resolve(Candidate& candidate, HashMap<String, bool>& fileExists)
    {
        if (!candidate.font) {
            auto path = filePathFromPattern(candidate.pattern);
            bool exists = !path.isEmpty() && fileExists.ensure(path, [&] {
                return FileSystem::fileExists(path);
            }).iterator->value;
            if (exists) {
                int ttcIndex;
                if (FcPatternGetInteger(candidate.pattern, FC_INDEX, 0, &ttcIndex) != FcResultMatch)
                    ttcIndex = 0;
                candidate.font = std::optional<Font> { Font { WTF::move(path), ttcIndex } };
            } else
                candidate.font = std::optional<Font> { };
        }
        return *candidate.font ? &**candidate.font : nullptr;
    }

    FcFontSet* m_fontSet { nullptr };
    Vector<Candidate> m_candidates;
};

SkiaSystemFallbackFontCache::SkiaSystemFallbackFontCache() = default;

SkiaSystemFallbackFontCache::~SkiaSystemFallbackFontCache() = default;

sk_sp<SkTypeface> SkiaSystemFallbackFontCache::fontForCharacterCluster(const String& locale, StringView stringView)
{
    FcCharSet* charSet = FcCharSetCreate();
    bool hasNonIgnorableCharacters = false;
    for (char32_t character : stringView.codePoints()) {
        if (!isDefaultIgnorableCodePoint(character)) {
            FcCharSetAddChar(charSet, character);
            hasNonIgnorableCharacters = true;
        }
    }

    if (!hasNonIgnorableCharacters) {
        FcCharSetDestroy(charSet);
        return nullptr;
    }

    auto& fontSetCache = m_cache.ensure(locale.isNull() ? emptyString() : locale, [&] {
        return FontSetCache::create(locale);
    }).iterator->value;

    if (!fontSetCache) {
        FcCharSetDestroy(charSet);
        return nullptr;
    }

    const auto* font = fontSetCache->fontForCharacterCluster(charSet, m_fileExists);
    FcCharSetDestroy(charSet);
    if (!font)
        return nullptr;

    return m_typefaceCache.ensure({ font->path, font->ttcIndex }, [font] -> sk_sp<SkTypeface> {
        return FontCache::forCurrentThread().fontManager().makeFromFile(font->path.utf8().legacyCStringPointer(), font->ttcIndex);
    }).iterator->value;
}

void SkiaSystemFallbackFontCache::clear()
{
    m_cache.clear();
    m_typefaceCache.clear();
    m_fileExists.clear();
}

} // namepsace WebCore

#endif // USE(SKIA) && !OS(ANDROID) && !PLATFORM(WIN)
