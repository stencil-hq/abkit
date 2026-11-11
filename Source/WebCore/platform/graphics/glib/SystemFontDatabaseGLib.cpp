/*
 * Copyright (C) 2025 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "SystemFontDatabase.h"

#include "SystemSettings.h"
#include "WebKitFontFamilyNames.h"

namespace WebCore {

#if OS(MACOS)
// What the system font keywords of CSS (`font: caption` and the others) mean on a Mac: the system
// font at the sizes AppKit gives its controls, under the family names Safari reports for them.
auto SystemFontDatabase::platformSystemFontShorthandInfo(FontShorthand fontShorthand) -> SystemFontShorthandInfo
{
    AtomString systemFamily { "system-ui"_s };
    switch (fontShorthand) {
    case FontShorthand::Menu:
        return { AtomString { "-apple-menu"_s }, 13, normalWeightValue() };
    case FontShorthand::StatusBar:
        return { AtomString { "-apple-status-bar"_s }, 10, normalWeightValue() };
    case FontShorthand::SmallCaption:
    case FontShorthand::WebkitSmallControl:
        return { systemFamily, 11, normalWeightValue() };
    case FontShorthand::WebkitMiniControl:
        return { systemFamily, 9, normalWeightValue() };
    case FontShorthand::Caption:
    case FontShorthand::Icon:
    case FontShorthand::MessageBox:
    case FontShorthand::WebkitControl:
        return { systemFamily, 13, normalWeightValue() };
    }
    ASSERT_NOT_REACHED();
    return { systemFamily, 13, normalWeightValue() };
}
#else
auto SystemFontDatabase::platformSystemFontShorthandInfo(FontShorthand) -> SystemFontShorthandInfo
{
    auto& systemSettings = SystemSettings::singleton();
    auto systemFontFamily = systemSettings.fontFamily();
    return { systemFontFamily ? AtomString(*systemFontFamily) : WebKitFontFamilyNames::standardFamily, systemSettings.fontSize().value_or(16), FontSelectionValue(systemSettings.fontWeight().value_or(400)) };
}
#endif

void SystemFontDatabase::platformInvalidate()
{
}

} // namespace WebCore
