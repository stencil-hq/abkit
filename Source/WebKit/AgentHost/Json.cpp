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

#include "Json.h"

#include "Utilities.h"
#include <cmath>

namespace AgentHost {

static JSCContext* context()
{
    static JSCContext* context = jsc_context_new();
    return context;
}

// Drops what a failed call left on the context, so that it does not surface in a later one.
static void clearException()
{
    if (jsc_context_get_exception(context()))
        jsc_context_clear_exception(context());
}

Json::Json(const Json& other)
    : m_value(other.m_value)
{
    if (m_value)
        g_object_ref(m_value);
}

Json::Json(Json&& other)
    : m_value(std::exchange(other.m_value, nullptr))
{
}

Json::~Json()
{
    if (m_value)
        g_object_unref(m_value);
}

Json& Json::operator=(Json other)
{
    std::swap(m_value, other.m_value);
    return *this;
}

Json Json::adopt(JSCValue* value)
{
    if (value && jsc_value_is_undefined(value)) {
        g_object_unref(value);
        value = nullptr;
    }
    return Json(value);
}

Json Json::parse(std::string_view text)
{
    // The parser takes a C string and stops at a NUL, which JSON text does not hold.
    if (text.find('\0') != std::string_view::npos || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))
        return { };
    std::string terminated(text);
    auto* value = jsc_value_new_from_json(context(), terminated.c_str());
    if (!value)
        clearException();
    return adopt(value);
}

Json Json::object()
{
    return Json(jsc_value_new_object(context(), nullptr, nullptr));
}

Json Json::array()
{
    return Json(jsc_value_new_array_from_garray(context(), nullptr));
}

Json Json::null()
{
    return Json(jsc_value_new_null(context()));
}

Json Json::string(std::string_view text)
{
    // Text from the network (header values, cookie values) need not be UTF-8.
    GChars valid;
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        valid.reset(g_utf8_make_valid(text.data(), static_cast<gssize>(text.size())));
        text = valid.get();
    }
    auto* bytes = g_bytes_new(text.data(), text.size());
    auto* value = jsc_value_new_string_from_bytes(context(), bytes);
    g_bytes_unref(bytes);
    return Json(value);
}

Json Json::number(double number)
{
    // JSON has no text for these.
    if (!std::isfinite(number))
        return null();
    return Json(jsc_value_new_number(context(), number));
}

Json Json::boolean(bool value)
{
    return Json(jsc_value_new_boolean(context(), value));
}

bool Json::isNull() const
{
    return m_value && jsc_value_is_null(m_value);
}

bool Json::isString() const
{
    return m_value && jsc_value_is_string(m_value);
}

bool Json::isNumber() const
{
    return m_value && jsc_value_is_number(m_value);
}

bool Json::isBoolean() const
{
    return m_value && jsc_value_is_boolean(m_value);
}

bool Json::isArray() const
{
    return m_value && jsc_value_is_array(m_value);
}

bool Json::isObject() const
{
    return m_value && jsc_value_is_object(m_value) && !jsc_value_is_array(m_value);
}

std::string Json::toString() const
{
    if (!m_value)
        return { };
    auto* bytes = jsc_value_to_string_as_bytes(m_value);
    if (!bytes)
        return { };
    gsize size = 0;
    auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    std::string text(data ? data : "", size);
    g_bytes_unref(bytes);
    return text;
}

double Json::toNumber() const
{
    return m_value ? jsc_value_to_double(m_value) : 0;
}

bool Json::toBoolean() const
{
    return m_value && jsc_value_to_boolean(m_value);
}

Json Json::get(const char* name) const
{
    // This also finds what every object inherits ("toString"); the protocol has no member of such a name.
    if (!isObject())
        return { };
    return adopt(jsc_value_object_get_property(m_value, name));
}

unsigned Json::length() const
{
    if (!isArray())
        return 0;
    GRef<JSCValue> length = GRef<JSCValue>::adopt(jsc_value_object_get_property(m_value, "length"));
    return length ? static_cast<unsigned>(jsc_value_to_int32(length.get())) : 0;
}

Json Json::at(unsigned index) const
{
    if (!isArray())
        return { };
    return adopt(jsc_value_object_get_property_at_index(m_value, index));
}

Json& Json::set(const char* name, const Json& value)
{
    // Defined, not assigned: a member may be called "__proto__".
    if (isObject() && value)
        jsc_value_object_define_property_data(m_value, name, static_cast<JSCValuePropertyFlags>(JSC_VALUE_PROPERTY_CONFIGURABLE | JSC_VALUE_PROPERTY_ENUMERABLE | JSC_VALUE_PROPERTY_WRITABLE), value.m_value);
    return *this;
}

Json& Json::setString(const char* name, std::string_view text)
{
    return set(name, string(text));
}

Json& Json::setNumber(const char* name, double number)
{
    return set(name, Json::number(number));
}

Json& Json::setBoolean(const char* name, bool value)
{
    return set(name, boolean(value));
}

Json& Json::setNull(const char* name)
{
    return set(name, null());
}

void Json::remove(const char* name)
{
    if (isObject())
        jsc_value_object_delete_property(m_value, name);
}

void Json::append(const Json& value)
{
    if (isArray() && value)
        jsc_value_object_set_property_at_index(m_value, length(), value.m_value);
}

std::string Json::serialize() const
{
    if (!m_value)
        return "null";
    char* text = jsc_value_to_json(m_value, 0);
    if (!text) {
        clearException();
        return "null";
    }
    return takeString(text);
}

std::string Json::quote(std::string_view text)
{
    return string(text).serialize();
}

std::optional<std::string> Json::stringMember(const char* name) const
{
    auto value = get(name);
    if (!value.isString())
        return std::nullopt;
    return value.toString();
}

std::optional<double> Json::numberMember(const char* name) const
{
    auto value = get(name);
    if (!value.isNumber())
        return std::nullopt;
    return value.toNumber();
}

std::optional<bool> Json::booleanMember(const char* name) const
{
    auto value = get(name);
    if (!value.isBoolean())
        return std::nullopt;
    return value.toBoolean();
}

} // namespace AgentHost
