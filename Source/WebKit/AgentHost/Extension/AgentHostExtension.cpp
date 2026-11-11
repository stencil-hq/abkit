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
// inside the page.

#include "../ExtensionMessages.h"
#include <wpe/webkit-web-process-extension.h>

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

extern "C" G_MODULE_EXPORT void webkit_web_process_extension_initialize(WebKitWebProcessExtension* extension)
{
    g_signal_connect(extension, "page-created", G_CALLBACK(+[](WebKitWebProcessExtension*, WebKitWebPage* page, gpointer) {
        g_signal_connect(page, "console-message-sent", G_CALLBACK(consoleMessageSent), nullptr);
    }), nullptr);
}
