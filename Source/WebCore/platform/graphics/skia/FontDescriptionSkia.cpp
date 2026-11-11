/*
 * Copyright (C) 2024 Igalia S.L.
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
#include "FontCascadeDescription.h"

#if OS(MACOS)
#include "WebKitFontFamilyNames.h"
#include <wtf/text/StringView.h>
#endif

namespace WebCore {

unsigned FontCascadeDescription::effectiveFamilyCount() const
{
    return familyCount();
}

FontFamilySpecification FontCascadeDescription::effectiveFamilyAt(unsigned index) const
{
    return familyAt(index);
}

#if OS(MACOS)
// On a Mac, text in a known language gets the generic families Core Text has for that language,
// and only text without one gets the families of the settings. These are Core Text's answers.
struct GenericFamilies {
    ASCIILiteral language;
    ASCIILiteral serif;
    ASCIILiteral sansSerif;
    ASCIILiteral monospace;
    ASCIILiteral cursive;
    ASCIILiteral fantasy;
};

static constexpr GenericFamilies defaultGenericFamilies { ""_s, "Times"_s, "Helvetica"_s, "Menlo"_s, "Apple Chancery"_s, "Zapfino"_s };
static constexpr GenericFamilies traditionalChineseGenericFamilies { "zh"_s, "Songti TC"_s, "PingFang TC"_s, "Menlo"_s, "Songti TC"_s, "Songti TC"_s };
static constexpr GenericFamilies hongKongGenericFamilies { "zh"_s, "Songti TC"_s, "PingFang HK"_s, "Menlo"_s, "Songti TC"_s, "Songti TC"_s };
static constexpr GenericFamilies macaoGenericFamilies { "zh"_s, "Songti TC"_s, "PingFang MO"_s, "Menlo"_s, "Songti TC"_s, "Songti TC"_s };
static constexpr std::array genericFamiliesByLanguage {
    GenericFamilies { "ru"_s, "Times"_s, "Helvetica"_s, "Menlo"_s, "Snell Roundhand"_s, "Snell Roundhand"_s },
    GenericFamilies { "uk"_s, "Times"_s, "Helvetica"_s, "Menlo"_s, "Snell Roundhand"_s, "Snell Roundhand"_s },
    GenericFamilies { "bg"_s, "Times"_s, "Helvetica"_s, "Menlo"_s, "Snell Roundhand"_s, "Snell Roundhand"_s },
    GenericFamilies { "sr"_s, "Times"_s, "Helvetica"_s, "Menlo"_s, "Snell Roundhand"_s, "Snell Roundhand"_s },
    GenericFamilies { "tr"_s, "Times"_s, "Helvetica"_s, "Menlo"_s, "Snell Roundhand"_s, "Snell Roundhand"_s },
    GenericFamilies { "he"_s, "Times"_s, "Lucida Grande"_s, "Courier New"_s, "Apple Chancery"_s, "Papyrus"_s },
    GenericFamilies { "ja"_s, "Hiragino Mincho ProN"_s, "Hiragino Sans"_s, "Menlo"_s, "Hiragino Mincho ProN"_s, "Hiragino Mincho ProN"_s },
    GenericFamilies { "zh"_s, "Songti SC"_s, "PingFang SC"_s, "Menlo"_s, "Songti SC"_s, "Songti SC"_s },
    GenericFamilies { "ko"_s, "AppleMyungjo"_s, "Apple SD Gothic Neo"_s, "Menlo"_s, "AppleMyungjo"_s, "AppleMyungjo"_s },
    GenericFamilies { "ar"_s, "Geeza Pro"_s, "Geeza Pro"_s, "Menlo"_s, "Geeza Pro"_s, "Geeza Pro"_s },
    GenericFamilies { "fa"_s, "Geeza Pro"_s, "Geeza Pro"_s, "Menlo"_s, "Geeza Pro"_s, "Geeza Pro"_s },
    GenericFamilies { "th"_s, "Thonburi"_s, "Thonburi"_s, "Menlo"_s, "Thonburi"_s, "Thonburi"_s },
    GenericFamilies { "hi"_s, "ITF Devanagari"_s, "Kohinoor Devanagari"_s, "Menlo"_s, "Kohinoor Devanagari"_s, "Kohinoor Devanagari"_s },
    GenericFamilies { "bn"_s, "Kohinoor Bangla"_s, "Kohinoor Bangla"_s, "Menlo"_s, "Kohinoor Bangla"_s, "Kohinoor Bangla"_s },
    GenericFamilies { "ta"_s, "Tamil Sangam MN"_s, "Tamil Sangam MN"_s, "Menlo"_s, "Tamil Sangam MN"_s, "Tamil Sangam MN"_s },
    GenericFamilies { "ka"_s, "Helvetica"_s, "Helvetica"_s, "Menlo"_s, "Helvetica"_s, "Helvetica"_s },
    GenericFamilies { "my"_s, "Noto Serif Myanmar"_s, "Noto Sans Myanmar"_s, "Menlo"_s, "Noto Serif Myanmar"_s, "Noto Serif Myanmar"_s },
    GenericFamilies { "km"_s, "Khmer MN"_s, "Khmer Sangam MN"_s, "Menlo"_s, "Khmer MN"_s, "Khmer MN"_s },
    GenericFamilies { "am"_s, "Kefa III"_s, "Kefa III"_s, "Menlo"_s, "Kefa III"_s, "Kefa III"_s },
};

static const GenericFamilies& genericFamiliesForLocale(const String& locale)
{
    auto lowercaseLocale = locale.convertToASCIILowercase();
    auto language = StringView(lowercaseLocale).left(std::min<size_t>(lowercaseLocale.find('-'), lowercaseLocale.length()));
    if (language == "zh"_s) {
        if (lowercaseLocale.contains("-hk"_s))
            return hongKongGenericFamilies;
        if (lowercaseLocale.contains("-mo"_s))
            return macaoGenericFamilies;
        if (lowercaseLocale.contains("-tw"_s) || lowercaseLocale.contains("-hant"_s))
            return traditionalChineseGenericFamilies;
    }
    for (auto& families : genericFamiliesByLanguage) {
        if (language == families.language)
            return families;
    }
    return defaultGenericFamilies;
}

AtomString FontDescription::platformResolveGenericFamily(UScriptCode script, const AtomString& locale, const AtomString& familyName)
{
    if (script == USCRIPT_COMMON || locale.isNull())
        return nullAtom();

    auto& families = genericFamiliesForLocale(locale.string());
    // Text without a family of its own is set in the serif family of Chinese, Japanese and Korean,
    // and in the standard font of the settings for every other language.
    if (familyName == WebKitFontFamilyNames::standardFamily) {
        if (families.language == "zh"_s || families.language == "ja"_s || families.language == "ko"_s)
            return AtomString { families.serif };
        return nullAtom();
    }
    if (familyName == WebKitFontFamilyNames::serifFamily)
        return AtomString { families.serif };
    if (familyName == WebKitFontFamilyNames::sansSerifFamily)
        return AtomString { families.sansSerif };
    if (familyName == WebKitFontFamilyNames::monospaceFamily)
        return AtomString { families.monospace };
    if (familyName == WebKitFontFamilyNames::cursiveFamily)
        return AtomString { families.cursive };
    if (familyName == WebKitFontFamilyNames::fantasyFamily)
        return AtomString { families.fantasy };
    return nullAtom();
}
#endif

}

