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

#include "Block.h"

#include "Keys.h"
#include <algorithm>
#include <cmath>

namespace AgentHost {

// Trusted input: the events a seat's devices would give the view, handed to
// it directly. The engine queues pointer and key events and sends the page
// the next only once it handled the last, while scripts go out at once: each
// step is followed by a script round trip, which comes back only after the
// page handled the step, and the answer waits for two animation frames after
// the last one, when what the input changed has been laid out and drawn.

// How long a step waits for the page (a page held by a dialog answers nothing).
static constexpr unsigned patience = 500;
// The engine multiplies precise scroll deltas by this.
static constexpr double scrollMultiplier = 2.5;
static const char settleScript[] = "await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));";

struct Block::InputStep {
    enum class Kind { Move, Button, Wheel, Key, Text };
    Kind kind { Kind::Move };
    double x { 0 };
    double y { 0 };
    double deltaX { 0 };
    double deltaY { 0 };
    // WPE_BUTTON_*, and the bit of the button among the pressed ones.
    unsigned button { 0 };
    unsigned buttonBit { 0 };
    bool pressed { false };
    unsigned clicks { 0 };
    // WPE_MODIFIER_KEYBOARD_*.
    unsigned modifiers { 0 };
    KeyStroke stroke;
    std::string text;
};

struct Block::InputPlayer {
    std::deque<InputStep> steps;
    Reply reply;
};

static std::string describe(const Json& value)
{
    return value ? value.serialize() : std::string("null");
}

static bool readModifiers(const Json& step, unsigned& modifiers, std::string& error)
{
    modifiers = 0;
    auto list = step.get("mods");
    if (!list || list.isNull())
        return true;
    if (!list.isArray()) {
        error = "input: `mods` must be an array";
        return false;
    }
    for (unsigned i = 0; i < list.length(); i++) {
        auto entry = list.at(i);
        if (!entry.isString()) {
            error = "input: `mods` holds strings";
            return false;
        }
        auto name = entry.toString();
        if (name == "shift")
            modifiers |= WPE_MODIFIER_KEYBOARD_SHIFT;
        else if (name == "ctrl" || name == "control")
            modifiers |= WPE_MODIFIER_KEYBOARD_CONTROL;
        else if (name == "alt" || name == "option")
            modifiers |= WPE_MODIFIER_KEYBOARD_ALT;
        else if (name == "meta" || name == "cmd" || name == "command")
            modifiers |= WPE_MODIFIER_KEYBOARD_META;
        else {
            error = "input: unknown modifier " + Json::quote(name) + " (shift, ctrl, alt, meta)";
            return false;
        }
    }
    return true;
}

static bool readNumber(const Json& step, const char* name, bool required, double& value, std::string& error)
{
    auto member = step.get(name);
    if ((!member || member.isNull()) && !required)
        return true;
    if (!member.isNumber() || !std::isfinite(member.toNumber())) {
        error = std::string("`") + name + "` must be a number";
        return false;
    }
    value = member.toNumber();
    return true;
}

void Block::input(const Json& operation, const Reply& reply)
{
    auto list = operation.get("events");
    if (!list.isArray()) {
        reply->invalid("input: `events` must be an array");
        return;
    }
    auto player = std::make_shared<InputPlayer>();
    std::string error;
    for (unsigned i = 0; i < list.length(); i++) {
        auto entry = list.at(i);
        auto type = entry.get("type");
        auto typeName = type.isString() ? type.toString() : std::string();
        InputStep step;
        if (typeName == "mouse") {
            auto action = entry.get("action");
            auto actionName = action.isString() ? action.toString() : std::string();
            if (actionName != "move" && actionName != "down" && actionName != "up") {
                reply->invalid("input: unknown mouse action " + describe(action) + " (move, down, up)");
                return;
            }
            step.kind = actionName == "move" ? InputStep::Kind::Move : InputStep::Kind::Button;
            step.pressed = actionName == "down";
            auto button = entry.get("button");
            auto buttonName = button.isString() ? button.toString() : std::string(button && !button.isNull() ? "" : "left");
            if (buttonName == "left") {
                step.button = WPE_BUTTON_PRIMARY;
                step.buttonBit = WPE_MODIFIER_POINTER_BUTTON1;
            } else if (buttonName == "middle") {
                step.button = WPE_BUTTON_MIDDLE;
                step.buttonBit = WPE_MODIFIER_POINTER_BUTTON2;
            } else if (buttonName == "right") {
                step.button = WPE_BUTTON_SECONDARY;
                step.buttonBit = WPE_MODIFIER_POINTER_BUTTON3;
            } else {
                reply->invalid("input: unknown button " + describe(button) + " (left, right, middle)");
                return;
            }
            double clicks = step.kind == InputStep::Kind::Button ? 1 : 0;
            if (!readNumber(entry, "x", true, step.x, error) || !readNumber(entry, "y", true, step.y, error)
                || !readNumber(entry, "clicks", false, clicks, error) || !readModifiers(entry, step.modifiers, error)) {
                reply->invalid(error);
                return;
            }
            step.clicks = static_cast<unsigned>(std::clamp(clicks, 0.0, 16.0));
        } else if (typeName == "wheel") {
            step.kind = InputStep::Kind::Wheel;
            if (!readNumber(entry, "x", true, step.x, error) || !readNumber(entry, "y", true, step.y, error)
                || !readNumber(entry, "dx", false, step.deltaX, error) || !readNumber(entry, "dy", false, step.deltaY, error)
                || !readModifiers(entry, step.modifiers, error)) {
                reply->invalid(error);
                return;
            }
        } else if (typeName == "key") {
            step.kind = InputStep::Kind::Key;
            auto action = entry.get("action");
            auto actionName = action.isString() ? action.toString() : std::string();
            if (actionName != "down" && actionName != "up") {
                reply->invalid("input: unknown key action " + describe(action) + " (down, up)");
                return;
            }
            step.pressed = actionName == "down";
            auto key = entry.stringMember("key");
            if (!key) {
                reply->invalid("`key` must be a string");
                return;
            }
            std::optional<std::string> code;
            for (const char* name : { "code", "text" }) {
                auto member = entry.get(name);
                if (member && !member.isNull() && !member.isString()) {
                    reply->invalid(std::string("`") + name + "` must be a string");
                    return;
                }
                if (name[0] == 'c' && member.isString())
                    code = member.toString();
            }
            if (!readModifiers(entry, step.modifiers, error)) {
                reply->invalid(error);
                return;
            }
            // The text a press types is the engine's to say, from the keysym: a `text` of the step's own is not used.
            auto stroke = keyStroke(*key, code, error);
            if (!stroke) {
                reply->failed("input: " + error);
                return;
            }
            step.stroke = *stroke;
        } else if (typeName == "text") {
            step.kind = InputStep::Kind::Text;
            auto text = entry.stringMember("text");
            if (!text) {
                reply->invalid("`text` must be a string");
                return;
            }
            step.text = std::move(*text);
        } else {
            reply->invalid("input: unknown type " + describe(type) + " (mouse, wheel, key, text)");
            return;
        }
        player->steps.push_back(std::move(step));
    }
    player->reply = reply;
    reply->setUnansweredMessage("input: the page is gone");

    // Sends the next step, then waits for the page to have handled it; with none left, waits for it to draw and answers.
    auto advance = std::make_shared<std::function<void()>>();
    *advance = [weakThis = weak_from_this(), player, weakAdvance = std::weak_ptr(advance)] {
        auto block = weakThis.lock();
        if (!block || block->m_closed)
            return;
        if (player->steps.empty()) {
            block->runPatiently(settleScript, patience, [player] {
                player->reply->ok();
            });
            return;
        }
        auto step = std::move(player->steps.front());
        player->steps.pop_front();
        block->sendInput(step);
        block->runPatiently("", patience, [advance = weakAdvance.lock()] {
            if (advance)
                (*advance)();
        });
    };
    (*advance)();
}

void Block::sendInput(const InputStep& step)
{
    auto* view = m_wpeView;
    auto time = static_cast<guint32>(g_get_monotonic_time() / 1000);
    WPEEvent* event = nullptr;
    switch (step.kind) {
    case InputStep::Kind::Move: {
        auto previous = std::exchange(m_pointer, std::make_pair(step.x, step.y));
        double deltaX = previous ? step.x - previous->first : 0;
        double deltaY = previous ? step.y - previous->second : 0;
        event = wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, view, WPE_INPUT_SOURCE_MOUSE, time,
            static_cast<WPEModifiers>(step.modifiers | m_pressedButtons), step.x, step.y, deltaX, deltaY);
        break;
    }
    case InputStep::Kind::Button:
        m_pointer = std::make_pair(step.x, step.y);
        if (step.pressed)
            m_pressedButtons |= step.buttonBit;
        else
            m_pressedButtons &= ~step.buttonBit;
        event = wpe_event_pointer_button_new(step.pressed ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP, view, WPE_INPUT_SOURCE_MOUSE, time,
            static_cast<WPEModifiers>(step.modifiers | m_pressedButtons), step.button, step.x, step.y, step.pressed ? std::max(step.clicks, 1u) : 0);
        break;
    case InputStep::Kind::Wheel:
        // The engine's wheel deltas say how the content moves, the protocol's how the view does.
        event = wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, time, static_cast<WPEModifiers>(step.modifiers | m_pressedButtons),
            -step.deltaX / scrollMultiplier, -step.deltaY / scrollMultiplier, TRUE, FALSE, step.x, step.y);
        break;
    case InputStep::Kind::Key:
        event = wpe_event_keyboard_new(step.pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, view, WPE_INPUT_SOURCE_KEYBOARD, time,
            static_cast<WPEModifiers>(step.modifiers), step.stroke.keycode, step.stroke.keysym);
        break;
    case InputStep::Kind::Text:
        // Typed as an input method commits it: inserted where the caret is, with no key events.
        webkit_web_view_execute_editing_command_with_argument(m_view.get(), "InsertText", step.text.c_str());
        return;
    }
    if (!event)
        return;
    wpe_view_event(view, event);
    wpe_event_unref(event);
}

} // namespace AgentHost
