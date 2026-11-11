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

#include "Keys.h"

#include <cstdlib>
#include <cstring>
#include <glib.h>
#include <wpe/wpe-platform.h>

namespace AgentHost {

struct CodeEntry {
    const char* code;
    unsigned evdev;
};

// DOM `code` values and the evdev codes of their keys.
static const CodeEntry codes[] = {
    { "Escape", 1 }, { "Digit1", 2 }, { "Digit2", 3 }, { "Digit3", 4 }, { "Digit4", 5 }, { "Digit5", 6 },
    { "Digit6", 7 }, { "Digit7", 8 }, { "Digit8", 9 }, { "Digit9", 10 }, { "Digit0", 11 }, { "Minus", 12 },
    { "Equal", 13 }, { "Backspace", 14 }, { "Tab", 15 }, { "KeyQ", 16 }, { "KeyW", 17 }, { "KeyE", 18 },
    { "KeyR", 19 }, { "KeyT", 20 }, { "KeyY", 21 }, { "KeyU", 22 }, { "KeyI", 23 }, { "KeyO", 24 },
    { "KeyP", 25 }, { "BracketLeft", 26 }, { "BracketRight", 27 }, { "Enter", 28 }, { "ControlLeft", 29 },
    { "KeyA", 30 }, { "KeyS", 31 }, { "KeyD", 32 }, { "KeyF", 33 }, { "KeyG", 34 }, { "KeyH", 35 },
    { "KeyJ", 36 }, { "KeyK", 37 }, { "KeyL", 38 }, { "Semicolon", 39 }, { "Quote", 40 }, { "Backquote", 41 },
    { "ShiftLeft", 42 }, { "Backslash", 43 }, { "KeyZ", 44 }, { "KeyX", 45 }, { "KeyC", 46 }, { "KeyV", 47 },
    { "KeyB", 48 }, { "KeyN", 49 }, { "KeyM", 50 }, { "Comma", 51 }, { "Period", 52 }, { "Slash", 53 },
    { "ShiftRight", 54 }, { "NumpadMultiply", 55 }, { "AltLeft", 56 }, { "Space", 57 }, { "CapsLock", 58 },
    { "F1", 59 }, { "F2", 60 }, { "F3", 61 }, { "F4", 62 }, { "F5", 63 }, { "F6", 64 }, { "F7", 65 },
    { "F8", 66 }, { "F9", 67 }, { "F10", 68 }, { "NumLock", 69 }, { "ScrollLock", 70 }, { "Numpad7", 71 },
    { "Numpad8", 72 }, { "Numpad9", 73 }, { "NumpadSubtract", 74 }, { "Numpad4", 75 }, { "Numpad5", 76 },
    { "Numpad6", 77 }, { "NumpadAdd", 78 }, { "Numpad1", 79 }, { "Numpad2", 80 }, { "Numpad3", 81 },
    { "Numpad0", 82 }, { "NumpadDecimal", 83 }, { "IntlBackslash", 86 }, { "F11", 87 }, { "F12", 88 },
    { "IntlRo", 89 }, { "Convert", 92 }, { "KanaMode", 93 }, { "NonConvert", 94 }, { "NumpadEnter", 96 },
    { "ControlRight", 97 }, { "NumpadDivide", 98 }, { "PrintScreen", 99 }, { "AltRight", 100 }, { "Home", 102 },
    { "ArrowUp", 103 }, { "PageUp", 104 }, { "ArrowLeft", 105 }, { "ArrowRight", 106 }, { "End", 107 },
    { "ArrowDown", 108 }, { "PageDown", 109 }, { "Insert", 110 }, { "Delete", 111 }, { "AudioVolumeMute", 113 },
    { "AudioVolumeDown", 114 }, { "AudioVolumeUp", 115 }, { "Power", 116 }, { "NumpadEqual", 117 },
    { "Pause", 119 }, { "NumpadComma", 121 }, { "Lang1", 122 }, { "Lang2", 123 }, { "IntlYen", 124 },
    { "MetaLeft", 125 }, { "MetaRight", 126 }, { "ContextMenu", 127 }, { "BrowserStop", 128 }, { "Again", 129 },
    { "Props", 130 }, { "Undo", 131 }, { "Select", 132 }, { "Copy", 133 }, { "Open", 134 }, { "Paste", 135 },
    { "Find", 136 }, { "Cut", 137 }, { "Help", 138 }, { "Sleep", 142 }, { "WakeUp", 143 },
    { "BrowserSearch", 217 }, { "BrowserFavorites", 156 }, { "BrowserBack", 158 }, { "BrowserForward", 159 },
    { "Eject", 161 }, { "MediaTrackNext", 163 }, { "MediaPlayPause", 164 }, { "MediaTrackPrevious", 165 },
    { "MediaStop", 166 }, { "BrowserRefresh", 173 }, { "F13", 183 }, { "F14", 184 }, { "F15", 185 },
    { "F16", 186 }, { "F17", 187 }, { "F18", 188 }, { "F19", 189 }, { "F20", 190 }, { "F21", 191 },
    { "F22", 192 }, { "F23", 193 }, { "F24", 194 },
};

static std::optional<unsigned> evdevFromCode(const char* code)
{
    for (auto& entry : codes) {
        if (!strcmp(entry.code, code))
            return entry.evdev;
    }
    return std::nullopt;
}

// The DOM `code` of the US layout's key typing an ASCII character, shifted or not.
static const char* usKey(gunichar character)
{
    static const struct {
        const char* characters;
        const char* code;
    } keys[] = {
        { "aA", "KeyA" }, { "bB", "KeyB" }, { "cC", "KeyC" }, { "dD", "KeyD" }, { "eE", "KeyE" }, { "fF", "KeyF" },
        { "gG", "KeyG" }, { "hH", "KeyH" }, { "iI", "KeyI" }, { "jJ", "KeyJ" }, { "kK", "KeyK" }, { "lL", "KeyL" },
        { "mM", "KeyM" }, { "nN", "KeyN" }, { "oO", "KeyO" }, { "pP", "KeyP" }, { "qQ", "KeyQ" }, { "rR", "KeyR" },
        { "sS", "KeyS" }, { "tT", "KeyT" }, { "uU", "KeyU" }, { "vV", "KeyV" }, { "wW", "KeyW" }, { "xX", "KeyX" },
        { "yY", "KeyY" }, { "zZ", "KeyZ" }, { "1!", "Digit1" }, { "2@", "Digit2" }, { "3#", "Digit3" },
        { "4$", "Digit4" }, { "5%", "Digit5" }, { "6^", "Digit6" }, { "7&", "Digit7" }, { "8*", "Digit8" },
        { "9(", "Digit9" }, { "0)", "Digit0" }, { "-_", "Minus" }, { "=+", "Equal" }, { "[{", "BracketLeft" },
        { "]}", "BracketRight" }, { "\\|", "Backslash" }, { ";:", "Semicolon" }, { "'\"", "Quote" },
        { "`~", "Backquote" }, { ",<", "Comma" }, { ".>", "Period" }, { "/?", "Slash" }, { " ", "Space" },
    };
    if (character >= 0x80)
        return nullptr;
    for (auto& key : keys) {
        if (strchr(key.characters, static_cast<int>(character)))
            return key.code;
    }
    return nullptr;
}

// The keypad keysym of a DOM `code` naming a keypad key.
static std::optional<unsigned> keypadKeysym(const std::string& code)
{
    static const char prefix[] = "Numpad";
    if (code.rfind(prefix, 0))
        return std::nullopt;
    std::string key = code.substr(strlen(prefix));
    if (key == "Enter")
        return WPE_KEY_KP_Enter;
    if (key == "Decimal")
        return WPE_KEY_KP_Decimal;
    if (key == "Add")
        return WPE_KEY_KP_Add;
    if (key == "Subtract")
        return WPE_KEY_KP_Subtract;
    if (key == "Multiply")
        return WPE_KEY_KP_Multiply;
    if (key == "Divide")
        return WPE_KEY_KP_Divide;
    if (key == "Equal")
        return WPE_KEY_KP_Equal;
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
        return WPE_KEY_KP_0 + static_cast<unsigned>(key[0] - '0');
    return std::nullopt;
}

struct NamedKey {
    const char* key;
    const char* code;
    unsigned keysym;
};

// DOM `key` values that are not characters; a modifier's right key is next to its left one.
static const NamedKey namedKeys[] = {
    { "Enter", "Enter", WPE_KEY_Return },
    { "Tab", "Tab", WPE_KEY_Tab },
    { "Backspace", "Backspace", WPE_KEY_BackSpace },
    { "Escape", "Escape", WPE_KEY_Escape },
    { "Delete", "Delete", WPE_KEY_Delete },
    { "Home", "Home", WPE_KEY_Home },
    { "End", "End", WPE_KEY_End },
    { "PageUp", "PageUp", WPE_KEY_Prior },
    { "PageDown", "PageDown", WPE_KEY_Next },
    { "ArrowUp", "ArrowUp", WPE_KEY_Up },
    { "ArrowDown", "ArrowDown", WPE_KEY_Down },
    { "ArrowLeft", "ArrowLeft", WPE_KEY_Left },
    { "ArrowRight", "ArrowRight", WPE_KEY_Right },
    { "Insert", "Insert", WPE_KEY_Insert },
    { "Help", "Insert", WPE_KEY_Help },
    { "ContextMenu", "ContextMenu", WPE_KEY_Menu },
    { "Clear", "NumLock", WPE_KEY_Clear },
    { "NumLock", "NumLock", WPE_KEY_Num_Lock },
    { "CapsLock", "CapsLock", WPE_KEY_Caps_Lock },
    { "AltGraph", "AltRight", WPE_KEY_ISO_Level3_Shift },
};

static const struct {
    const char* key;
    NamedKey left;
    NamedKey right;
} modifierKeys[] = {
    { "Shift", { "Shift", "ShiftLeft", WPE_KEY_Shift_L }, { "Shift", "ShiftRight", WPE_KEY_Shift_R } },
    { "Control", { "Control", "ControlLeft", WPE_KEY_Control_L }, { "Control", "ControlRight", WPE_KEY_Control_R } },
    { "Alt", { "Alt", "AltLeft", WPE_KEY_Alt_L }, { "Alt", "AltRight", WPE_KEY_Alt_R } },
    { "Meta", { "Meta", "MetaLeft", WPE_KEY_Super_L }, { "Meta", "MetaRight", WPE_KEY_Super_R } },
};

static std::optional<KeyStroke> namedStroke(const std::string& key, const std::optional<std::string>& code)
{
    auto stroke = [](const NamedKey& named) {
        return KeyStroke { evdevFromCode(named.code).value_or(0) + 8, named.keysym };
    };
    for (auto& named : namedKeys) {
        if (key == named.key)
            return stroke(named);
    }
    bool right = code && code->size() >= 5 && !code->compare(code->size() - 5, 5, "Right");
    for (auto& modifier : modifierKeys) {
        if (key == modifier.key)
            return stroke(right ? modifier.right : modifier.left);
    }
    // F1 to F35.
    if (key.size() >= 2 && key.size() <= 3 && key[0] == 'F' && g_ascii_isdigit(key[1]) && (key.size() == 2 || g_ascii_isdigit(key[2]))) {
        unsigned number = static_cast<unsigned>(atoi(key.c_str() + 1));
        if (number >= 1 && number <= 35)
            return KeyStroke { evdevFromCode(key.c_str()).value_or(0) + 8, WPE_KEY_F1 + number - 1 };
    }
    return std::nullopt;
}

static std::optional<KeyStroke> characterStroke(const std::string& key)
{
    if (key.empty() || !g_utf8_validate(key.data(), static_cast<gssize>(key.size()), nullptr) || g_utf8_strlen(key.data(), static_cast<gssize>(key.size())) != 1)
        return std::nullopt;
    gunichar character = g_utf8_get_char(key.data());
    // Latin-1 characters are their own keysyms, the rest Unicode keysyms.
    unsigned keysym = (character >= 0x20 && character <= 0x7e) || (character >= 0xa0 && character <= 0xff) ? character : (0x01000000 | character);
    const char* code = usKey(character);
    return KeyStroke { (code ? evdevFromCode(code).value_or(0) : 0) + 8, keysym };
}

static std::string quoted(const std::string& text)
{
    std::string result = "\"";
    for (char character : text) {
        if (character == '"' || character == '\\')
            result += '\\';
        result += character;
    }
    return result + '"';
}

std::optional<KeyStroke> keyStroke(const std::string& key, const std::optional<std::string>& code, std::string& error)
{
    auto stroke = namedStroke(key, code);
    if (!stroke)
        stroke = characterStroke(key);
    if (!stroke) {
        error = "unknown key " + quoted(key);
        return std::nullopt;
    }
    if (code) {
        auto evdev = evdevFromCode(code->c_str());
        if (!evdev) {
            error = "unknown key code " + quoted(*code);
            return std::nullopt;
        }
        stroke->keycode = *evdev + 8;
        if (auto keypad = keypadKeysym(*code))
            stroke->keysym = *keypad;
    }
    return stroke;
}

} // namespace AgentHost
