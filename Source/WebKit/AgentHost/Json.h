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

// JSON through the JavaScriptCore GLib API: text is parsed into and written
// from JSCValues of one context the host keeps for that purpose (no script
// ever runs in it).

#include <jsc/jsc.h>
#include <optional>
#include <string>
#include <string_view>

namespace AgentHost {

class Json {
public:
    // No value: what a missing member reads as.
    Json() = default;
    Json(const Json&);
    Json(Json&&);
    ~Json();
    Json& operator=(Json);

    // The value text stands for, or no value when text is not JSON.
    static Json parse(std::string_view text);
    // A value of the host's context (undefined reads as no value). Takes over the reference.
    static Json adopt(JSCValue*);

    static Json object();
    static Json array();
    static Json null();
    static Json string(std::string_view);
    static Json number(double);
    static Json boolean(bool);

    explicit operator bool() const { return m_value; }
    bool isNull() const;
    bool isString() const;
    bool isNumber() const;
    bool isBoolean() const;
    bool isArray() const;
    // An object that is not an array.
    bool isObject() const;

    std::string toString() const;
    double toNumber() const;
    bool toBoolean() const;

    // Member name of an object; no value when missing or when this is not an object.
    Json get(const char* name) const;
    unsigned length() const;
    Json at(unsigned index) const;

    Json& set(const char* name, const Json&);
    Json& setString(const char* name, std::string_view);
    Json& setNumber(const char* name, double);
    Json& setBoolean(const char* name, bool);
    Json& setNull(const char* name);
    void remove(const char* name);
    void append(const Json&);

    // The JSON text of the value.
    std::string serialize() const;

    // The JSON text of a string.
    static std::string quote(std::string_view);

    // Typed reads of an object's members. Each returns nothing for a missing member.
    std::optional<std::string> stringMember(const char* name) const;
    std::optional<double> numberMember(const char* name) const;
    std::optional<bool> booleanMember(const char* name) const;

private:
    explicit Json(JSCValue* value)
        : m_value(value)
    {
    }

    JSCValue* m_value { nullptr };
};

} // namespace AgentHost
