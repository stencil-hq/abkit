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

#include "ExtensionMessages.h"
#include <cstring>
#include <libsoup/soup.h>

namespace AgentHost {

// What the block sees of its pages from outside them: the responses to their
// navigations, every resource they load, what they write to their consoles
// and what they download.

// The headers of a request or a response; those sent more than once are joined.
static Json headersObject(SoupMessageHeaders* messageHeaders)
{
    std::vector<std::pair<std::string, std::string>> headers;
    if (messageHeaders) {
        soup_message_headers_foreach(messageHeaders, [](const char* name, const char* value, gpointer data) {
            auto& headers = *static_cast<std::vector<std::pair<std::string, std::string>>*>(data);
            for (auto& header : headers) {
                if (header.first == name) {
                    header.second.append(", ").append(value);
                    return;
                }
            }
            headers.emplace_back(name, value);
        }, &headers);
    }
    auto object = Json::object();
    for (auto& [name, value] : headers)
        object.setString(name.c_str(), value);
    return object;
}

bool Block::decideResponse(WebKitResponsePolicyDecision* decision)
{
    auto* response = webkit_response_policy_decision_get_response(decision);
    bool shows = webkit_response_policy_decision_is_mime_type_supported(decision);
    bool attachment = false;
    if (auto* headers = response ? webkit_uri_response_get_http_headers(response) : nullptr) {
        if (const char* disposition = soup_message_headers_get_one(headers, "Content-Disposition")) {
            GChars lowered(g_ascii_strdown(disposition, -1));
            attachment = g_str_has_prefix(g_strchug(lowered.get()), "attachment");
        }
    }

    // A response that becomes a download or is dropped may never be its resource's: it is reported from here.
    bool main = webkit_response_policy_decision_is_main_frame_main_resource(decision);
    std::string address = stringOrEmpty(response ? webkit_uri_response_get_uri(response) : nullptr);
    Resource* answered = nullptr;
    for (auto& resource : m_resources) {
        if ((main ? resource.main : resource.url == address) && (!answered || resource.identifier > answered->identifier))
            answered = &resource;
    }
    if (answered && response)
        resourceResponded(*answered, response);

    // Shown, saved (while downloads are, what is an attachment or cannot be shown) or dropped.
    auto* policyDecision = WEBKIT_POLICY_DECISION(decision);
    if (m_downloadDirectory && (attachment || !shows))
        webkit_policy_decision_download(policyDecision);
    else if (shows)
        webkit_policy_decision_use(policyDecision);
    else
        webkit_policy_decision_ignore(policyDecision);
    return true;
}

// Resource loads, as the engine reports them to the view: nothing in the page watches them.

Block::Resource* Block::findResource(WebKitWebResource* resource)
{
    for (auto& entry : m_resources) {
        if (entry.resource.get() == resource)
            return &entry;
    }
    return nullptr;
}

void Block::reportRequest(const Resource& resource, WebKitURIRequest* request)
{
    auto event = timedEventOfType("request");
    event.setNumber("id", static_cast<double>(resource.identifier));
    event.setString("url", resource.url);
    event.setString("method", stringOrEmpty(webkit_uri_request_get_http_method(request)));
    event.set("headers", headersObject(webkit_uri_request_get_http_headers(request)));
    event.setBoolean("main", resource.main);
    record(std::move(event));
}

void Block::resourceLoadStarted(WebKitWebResource* resource, WebKitURIRequest* request)
{
    Resource entry;
    entry.resource = GRef<WebKitWebResource>::retain(resource);
    entry.identifier = ++m_lastResource;
    entry.main = resource == webkit_web_view_get_main_resource(m_view.get());
    entry.url = stringOrEmpty(webkit_uri_request_get_uri(request));

    // (What `allow` refuses never gets here: a navigation ends at its policy decision, reported as blocked, and
    // the content filter stops anything else before its load starts.)
    reportRequest(entry, request);
    m_resources.push_back(std::move(entry));

    // Sent again with the response that redirected it when the load goes on at another address.
    g_signal_connect(resource, "sent-request", G_CALLBACK(+[](WebKitWebResource* resource, WebKitURIRequest* request, WebKitURIResponse* redirection, gpointer block) {
        auto* entry = static_cast<Block*>(block)->findResource(resource);
        if (!entry || !redirection)
            return;
        entry->url = stringOrEmpty(webkit_uri_request_get_uri(request));
        static_cast<Block*>(block)->reportRequest(*entry, request);
    }), this);
    g_signal_connect(resource, "notify::response", G_CALLBACK(+[](WebKitWebResource* resource, GParamSpec*, gpointer block) {
        auto* response = webkit_web_resource_get_response(resource);
        auto* entry = static_cast<Block*>(block)->findResource(resource);
        if (entry && response)
            static_cast<Block*>(block)->resourceResponded(*entry, response);
    }), this);
    g_signal_connect(resource, "failed", G_CALLBACK(+[](WebKitWebResource* resource, GError* error, gpointer block) {
        static_cast<Block*>(block)->resourceFailed(resource, error && error->message ? error->message : "the load failed");
    }), this);
    g_signal_connect(resource, "failed-with-tls-errors", G_CALLBACK(+[](WebKitWebResource* resource, GTlsCertificate*, GTlsCertificateFlags, gpointer block) {
        static_cast<Block*>(block)->resourceFailed(resource, "the server's certificate is not trusted");
    }), this);
    g_signal_connect(resource, "finished", G_CALLBACK(+[](WebKitWebResource* resource, gpointer block) {
        static_cast<Block*>(block)->resourceFinished(resource);
    }), this);
}

void Block::resourceResponded(Resource& resource, WebKitURIResponse* response)
{
    if (std::exchange(resource.responded, true))
        return;
    auto event = timedEventOfType("response");
    event.setNumber("id", static_cast<double>(resource.identifier));
    event.setString("url", stringOrEmpty(webkit_uri_response_get_uri(response)));
    event.setNumber("status", webkit_uri_response_get_status_code(response));
    event.setString("mime", stringOrEmpty(webkit_uri_response_get_mime_type(response)));
    event.set("headers", headersObject(webkit_uri_response_get_http_headers(response)));
    event.setBoolean("main", resource.main);
    record(std::move(event));
}

void Block::resourceFailed(WebKitWebResource* resource, const char* error)
{
    auto* entry = findResource(resource);
    if (!entry || std::exchange(entry->failed, true))
        return;
    auto event = timedEventOfType("requestFailed");
    event.setNumber("id", static_cast<double>(entry->identifier));
    event.setString("url", entry->url);
    event.setString("error", error);
    record(std::move(event));
}

// The end of a load, which follows a failure too.
void Block::resourceFinished(WebKitWebResource* resource)
{
    for (auto entry = m_resources.begin(); entry != m_resources.end(); ++entry) {
        if (entry->resource.get() != resource)
            continue;
        auto ended = std::move(*entry);
        m_resources.erase(entry);
        g_signal_handlers_disconnect_by_data(resource, this);
        if (ended.failed)
            return;
        // The engine does not count the bytes of a load for its embedder: the length is the one the response declared.
        auto* response = webkit_web_resource_get_response(resource);
        auto event = timedEventOfType("requestFinished");
        event.setNumber("id", static_cast<double>(ended.identifier));
        event.setNumber("length", response ? static_cast<double>(webkit_uri_response_get_content_length(response)) : 0);
        record(std::move(event));
        return;
    }
}

// What the host's web process extension says of the page.
bool Block::userMessageReceived(WebKitUserMessage* message)
{
    if (strcmp(webkit_user_message_get_name(message), consoleMessageName))
        return false;
    auto* parameters = webkit_user_message_get_parameters(message);
    if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE(consoleMessageFormat)))
        return true;
    const char* level;
    const char* source;
    const char* text;
    const char* address;
    guint32 line;
    g_variant_get(parameters, "(&s&s&s&su)", &level, &source, &text, &address, &line);
    auto event = timedEventOfType("console");
    event.setString("level", level);
    event.setString("text", text);
    event.setString("source", source);
    if (*address)
        event.setString("url", address);
    if (line)
        event.setNumber("line", line);
    record(std::move(event));
    return true;
}

// Downloads: saved into the downloads directory while there is one, else cancelled.

void Block::downloadStarted(WebKitDownload* download)
{
    if (webkit_download_get_web_view(download) != m_view.get())
        return;
    if (!m_downloadDirectory) {
        webkit_download_cancel(download);
        return;
    }
    auto* request = webkit_download_get_request(download);
    m_downloads.push_back({ GRef<WebKitDownload>::retain(download), ++m_lastDownload, stringOrEmpty(request ? webkit_uri_request_get_uri(request) : nullptr), std::nullopt });
    g_signal_connect(download, "decide-destination", G_CALLBACK(+[](WebKitDownload* download, const char* suggestedName, gpointer block) -> gboolean {
        return static_cast<Block*>(block)->decideDestination(download, suggestedName);
    }), this);
    g_signal_connect(download, "finished", G_CALLBACK(+[](WebKitDownload* download, gpointer block) {
        static_cast<Block*>(block)->downloadEnded(download, nullptr);
    }), this);
    g_signal_connect(download, "failed", G_CALLBACK(+[](WebKitDownload* download, GError* error, gpointer block) {
        static_cast<Block*>(block)->downloadEnded(download, error && error->message ? error->message : "the download failed");
    }), this);
}

// A path in `directory` for a file suggested as `name` that no file has yet: `name`, then `name (1)`, `name (2)`, ... before its extension.
static std::string uniquePath(const std::string& directory, const char* suggestedName)
{
    GChars base(g_path_get_basename(suggestedName && *suggestedName ? suggestedName : "download"));
    std::string name = base.get();
    if (name == "." || name == ".." || name == G_DIR_SEPARATOR_S)
        name = "download";
    GChars path(g_build_filename(directory.c_str(), name.c_str(), nullptr));
    if (!g_file_test(path.get(), G_FILE_TEST_EXISTS))
        return path.get();
    std::string stem = name;
    std::string extension;
    if (auto dot = name.rfind('.'); dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        extension = name.substr(dot);
    }
    for (unsigned number = 1; number < 100000; number++) {
        std::string numbered = stem + " (" + std::to_string(number) + ")" + extension;
        GChars candidate(g_build_filename(directory.c_str(), numbered.c_str(), nullptr));
        if (!g_file_test(candidate.get(), G_FILE_TEST_EXISTS))
            return candidate.get();
    }
    return path.get();
}

bool Block::decideDestination(WebKitDownload* download, const char* suggestedName)
{
    Download* entry = nullptr;
    for (auto& candidate : m_downloads) {
        if (candidate.download.get() == download)
            entry = &candidate;
    }
    if (!entry || !m_downloadDirectory || g_mkdir_with_parents(m_downloadDirectory->c_str(), 0755) < 0) {
        webkit_download_cancel(download);
        return true;
    }
    entry->path = uniquePath(*m_downloadDirectory, suggestedName);
    webkit_download_set_destination(download, entry->path->c_str());
    auto event = eventOfType("download");
    event.setNumber("id", static_cast<double>(entry->identifier));
    event.setString("url", entry->url);
    event.setString("path", *entry->path);
    event.setString("state", "started");
    record(std::move(event));
    return true;
}

void Block::downloadEnded(WebKitDownload* download, const char* error)
{
    for (auto entry = m_downloads.begin(); entry != m_downloads.end(); ++entry) {
        if (entry->download.get() != download)
            continue;
        auto ended = std::move(*entry);
        m_downloads.erase(entry);
        g_signal_handlers_disconnect_by_data(download, this);
        auto event = eventOfType("download");
        event.setNumber("id", static_cast<double>(ended.identifier));
        event.setString("url", ended.url);
        if (ended.path)
            event.setString("path", *ended.path);
        else
            event.setNull("path");
        event.setString("state", error ? "failed" : "finished");
        if (error)
            event.setString("error", error);
        record(std::move(event));
        return;
    }
}

} // namespace AgentHost
