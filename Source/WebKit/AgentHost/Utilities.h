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

// GLib conveniences for the host, which uses only the engine's public API
// and so has none of WTF's.

#include <functional>
#include <gio/gio.h>
#include <glib-object.h>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Diagnostics, on standard error; shown with G_MESSAGES_DEBUG=ABHost.
#define AGENT_HOST_DEBUG(...) g_log("ABHost", G_LOG_LEVEL_DEBUG, __VA_ARGS__)

namespace AgentHost {

// A strong reference to a GObject.
template<typename T> class GRef {
public:
    GRef() = default;
    GRef(const GRef& other)
        : m_object(other.m_object)
    {
        if (m_object)
            g_object_ref(m_object);
    }
    GRef(GRef&& other)
        : m_object(std::exchange(other.m_object, nullptr))
    {
    }
    ~GRef() { clear(); }

    GRef& operator=(GRef other)
    {
        std::swap(m_object, other.m_object);
        return *this;
    }

    // Takes over a reference the caller owns.
    static GRef adopt(T* object)
    {
        GRef result;
        result.m_object = object;
        return result;
    }

    // Takes a reference of its own.
    static GRef retain(T* object)
    {
        if (object)
            g_object_ref(object);
        return adopt(object);
    }

    T* get() const { return m_object; }
    explicit operator bool() const { return m_object; }

    void clear()
    {
        if (auto* object = std::exchange(m_object, nullptr))
            g_object_unref(object);
    }

private:
    T* m_object { nullptr };
};

struct GFreeDeleter {
    void operator()(void* pointer) const { g_free(pointer); }
};
using GChars = std::unique_ptr<char, GFreeDeleter>;

// The text of a string GLib allocated, which is freed.
inline std::string takeString(char* text)
{
    GChars owned(text);
    return text ? std::string(text) : std::string();
}

inline std::string stringOrEmpty(const char* text)
{
    return text ? std::string(text) : std::string();
}

// The message of an error, which is freed and cleared.
inline std::string takeError(GError*& error, const char* fallback)
{
    std::string message = error && error->message && *error->message ? error->message : fallback;
    g_clear_error(&error);
    return message;
}

// A GAsyncReadyCallback running a function once.
using AsyncFunction = std::function<void(GObject*, GAsyncResult*)>;

inline void asyncReady(GObject* source, GAsyncResult* result, gpointer data)
{
    std::unique_ptr<AsyncFunction> function(static_cast<AsyncFunction*>(data));
    (*function)(source, result);
}

inline gpointer asyncData(AsyncFunction&& function)
{
    return new AsyncFunction(std::move(function));
}

// Runs a function on the main loop after a delay; the identifier cancels it (g_source_remove()) until it ran.
inline guint runAfter(guint milliseconds, std::function<void()>&& function)
{
    return g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
    }, new std::function<void()>(std::move(function)), [](gpointer data) {
        delete static_cast<std::function<void()>*>(data);
    });
}

// Runs a function on the main loop of the default context, from any thread, after those handed over before it.
// (Not g_main_context_invoke(): called before the loop runs, that would run the function on the calling thread.)
inline void runOnMainLoop(std::function<void()>&& function)
{
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
    }, new std::function<void()>(std::move(function)), [](gpointer data) {
        delete static_cast<std::function<void()>*>(data);
    });
}

// A function several parties may ask to run, which runs once.
class Once {
public:
    explicit Once(std::function<void()>&& function)
        : m_function(std::make_shared<std::function<void()>>(std::move(function)))
    {
    }

    void operator()() const
    {
        if (auto function = std::exchange(*m_function, nullptr))
            function();
    }

private:
    std::shared_ptr<std::function<void()>> m_function;
};

} // namespace AgentHost
