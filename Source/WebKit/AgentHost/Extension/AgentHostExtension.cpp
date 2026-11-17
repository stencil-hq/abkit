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

// The host's web process extension: a module the engine loads into each web
// process (from the `extensions` directory beside the host's executable). It
// tells the host what pages write to their consoles, which the engine does
// not tell its embedder otherwise, so that no script has to listen for it
// inside the page. On Windows it also writes down a crash of the process in
// the graphics stack, for the host to draw its pages without the GPU from
// then on (Host::drawsWithGPU()).

#include "../ExtensionMessages.h"
#include <wpe/webkit-web-process-extension.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static const char* levelName(WebKitConsoleMessageLevel level)
{
    switch (level) {
    case WEBKIT_CONSOLE_MESSAGE_LEVEL_INFO:
        return "info";
    case WEBKIT_CONSOLE_MESSAGE_LEVEL_LOG:
        return "log";
    case WEBKIT_CONSOLE_MESSAGE_LEVEL_WARNING:
        return "warning";
    case WEBKIT_CONSOLE_MESSAGE_LEVEL_ERROR:
        return "error";
    case WEBKIT_CONSOLE_MESSAGE_LEVEL_DEBUG:
        return "debug";
    }
    return "log";
}

static const char* sourceName(WebKitConsoleMessageSource source)
{
    switch (source) {
    case WEBKIT_CONSOLE_MESSAGE_SOURCE_JAVASCRIPT:
        return "javascript";
    case WEBKIT_CONSOLE_MESSAGE_SOURCE_NETWORK:
        return "network";
    case WEBKIT_CONSOLE_MESSAGE_SOURCE_CONSOLE_API:
        return "console-api";
    case WEBKIT_CONSOLE_MESSAGE_SOURCE_SECURITY:
        return "security";
    case WEBKIT_CONSOLE_MESSAGE_SOURCE_OTHER:
        return "other";
    }
    return "other";
}

static void consoleMessageSent(WebKitWebPage* page, WebKitConsoleMessage* message, gpointer)
{
    // A variant's strings are UTF-8, which a page's text need not make.
    const char* text = webkit_console_message_get_text(message);
    const char* sourceID = webkit_console_message_get_source_id(message);
    char* validText = g_utf8_make_valid(text ? text : "", -1);
    char* validSourceID = g_utf8_make_valid(sourceID ? sourceID : "", -1);
    auto* parameters = g_variant_new(AgentHost::consoleMessageFormat, levelName(webkit_console_message_get_level(message)),
        sourceName(webkit_console_message_get_source(message)), validText, validSourceID, webkit_console_message_get_line(message));
    g_free(validText);
    g_free(validSourceID);
    // The message is the view's from here; no answer is waited for. (The engine marks user messages from a
    // page as deprecated without naming what replaces them: there is no other way from here to the view.)
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    webkit_web_page_send_message_to_view(page, webkit_user_message_new(AgentHost::consoleMessageName, parameters), nullptr, nullptr, nullptr);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

#if defined(_WIN32)

// The file the host named (UTF-16): a crash in the graphics stack writes the module's path into it.
static const wchar_t* graphicsCrashFile;
static LPTOP_LEVEL_EXCEPTION_FILTER previousExceptionFilter;

// The modules of the graphics stack outside the driver store: Direct3D and DXGI, WARP, and ANGLE.
static const wchar_t* const graphicsModules[] = {
    L"d3d11.dll", L"d3d12.dll", L"d3d12core.dll", L"dxgi.dll", L"d3d10warp.dll", L"libegl.dll", L"libglesv2.dll",
};

// Whether `text` starts with lowercase ASCII `prefix`, ignoring case; `whole`: and ends there.
static bool startsWithIgnoringASCIICase(const wchar_t* text, const wchar_t* prefix, bool whole)
{
    for (; *prefix; ++text, ++prefix) {
        wchar_t character = *text >= L'A' && *text <= L'Z' ? *text - L'A' + L'a' : *text;
        if (character != *prefix)
            return false;
    }
    return !whole || !*text;
}

// Whether the module at `path` is of the graphics stack: a display driver (Windows installs every user-mode
// display driver into its driver store) or one of `graphicsModules`.
static bool isGraphicsModule(const wchar_t* path)
{
    const wchar_t* name = path;
    for (const wchar_t* character = path; *character; ++character) {
        if (startsWithIgnoringASCIICase(character, L"\\driverstore\\", false))
            return true;
        if (*character == L'\\' || *character == L'/')
            name = character + 1;
    }
    for (auto* module : graphicsModules) {
        if (startsWithIgnoringASCIICase(name, module, true))
            return true;
    }
    return false;
}

// The process is going down with an exception nothing handled: when it was raised in the graphics stack, the
// module's path goes into the host's file. It allocates nothing: the crash may have broken the heap.
static LONG WINAPI graphicsCrashFilter(EXCEPTION_POINTERS* exception)
{
    // A stack overflow leaves no stack to work on.
    if (exception && exception->ExceptionRecord && exception->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH];
        auto address = static_cast<LPCWSTR>(exception->ExceptionRecord->ExceptionAddress);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, address, &module)) {
            DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
            if (length && length < MAX_PATH && isGraphicsModule(path)) {
                char text[MAX_PATH * 3];
                int size = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(length), text, sizeof(text), nullptr, nullptr);
                HANDLE file = CreateFileW(graphicsCrashFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(file, text, static_cast<DWORD>(size > 0 ? size : 0), &written, nullptr);
                    CloseHandle(file);
                }
            }
        }
    }
    return previousExceptionFilter ? previousExceptionFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
}

#endif

// `userData`: on Windows, the path of the file a crash in the graphics stack is written down in (a string).
extern "C" G_MODULE_EXPORT void webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension* extension, [[maybe_unused]] const GVariant* userData)
{
#if defined(_WIN32)
    auto* data = const_cast<GVariant*>(userData);
    if (data && g_variant_is_of_type(data, G_VARIANT_TYPE_STRING)) {
        // Kept for as long as the process runs.
        graphicsCrashFile = reinterpret_cast<const wchar_t*>(g_utf8_to_utf16(g_variant_get_string(data, nullptr), -1, nullptr, nullptr, nullptr));
        if (graphicsCrashFile)
            previousExceptionFilter = SetUnhandledExceptionFilter(graphicsCrashFilter);
    }
#endif
    g_signal_connect(extension, "page-created", G_CALLBACK(+[](WebKitWebProcessExtension*, WebKitWebPage* page, gpointer) {
        g_signal_connect(page, "console-message-sent", G_CALLBACK(consoleMessageSent), nullptr);
    }), nullptr);
}
