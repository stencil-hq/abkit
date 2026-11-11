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

#include "Host.h"

#include "Block.h"
#include "Display.h"
#include "ExtensionMessages.h"
#include "Platform.h"
#include <cmath>
#include <cstring>
#include <glib/gstdio.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace AgentHost {

Responder::Responder(std::weak_ptr<Transport> transport, const char* channel, std::string&& id)
    : m_transport(std::move(transport))
    , m_channel(channel)
    , m_id(std::move(id))
{
}

Responder::~Responder()
{
    if (!m_answered)
        failed(m_unanswered);
}

void Responder::send(std::string_view body)
{
    if (std::exchange(m_answered, true))
        return;
    auto transport = m_transport.lock();
    if (!transport)
        return;
    std::string frame;
    frame.reserve(body.size() + m_id.size() + 32);
    auto compose = [&](std::string_view answer) {
        frame.clear();
        frame.append("{\"id\":").append(m_id).append(",\"").append(m_channel).append("\":").append(answer).append("}");
    };
    compose(body);
    if (frame.size() > Transport::maxFrameSize)
        compose("{\"error\":{\"kind\":\"failed\",\"message\":\"the answer is too large for a frame\"}}");
    transport->send(frame);
}

void Responder::ok(std::string_view value)
{
    std::string body;
    body.reserve(value.size() + 8);
    body.append("{\"ok\":").append(value).append("}");
    send(body);
}

void Responder::fail(const char* kind, std::string_view message)
{
    auto error = Json::object();
    error.setString("kind", kind);
    error.setString("message", message);
    send("{\"error\":" + error.serialize() + "}");
}

// Removes a directory and what is in it, following no links.
static void removeTree(const char* path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        if (auto* directory = g_dir_open(path, 0, nullptr)) {
            while (const char* name = g_dir_read_name(directory)) {
                GChars child(g_build_filename(path, name, nullptr));
                removeTree(child.get());
            }
            g_dir_close(directory);
        }
    }
    g_remove(path);
}

Host::Host(std::string&& dataDirectory, std::string&& inspectorAddress, std::function<void(int)>&& quit)
    : m_dataDirectory(std::move(dataDirectory))
    , m_inspectorAddress(std::move(inspectorAddress))
    , m_quit(std::move(quit))
{
    GChars token(g_uuid_string_random());
    m_frameToken = std::string("stencil_") + token.get();
}

Host::~Host() = default;

bool Host::start(int input, int output)
{
    // The engine starts its remote inspector servers from this address, and only then: the host names it,
    // nothing is started without it.
    if (!m_inspectorAddress.empty())
        g_setenv("WEBKIT_INSPECTOR_HTTP_SERVER", m_inspectorAddress.c_str(), TRUE);

    // The engine has to be set up before the first JavaScriptCore context (Json's) exists: started the other way
    // round, this process never hears of its helper processes and no page loads. Making any WebKit object does
    // it, and the context is the first one made.
    m_context = GRef<WebKitWebContext>::adopt(webkit_web_context_new());
    g_signal_connect(m_context.get(), "initialize-web-process-extensions", G_CALLBACK(+[](WebKitWebContext* context, gpointer) {
        // The host's extension is in a directory beside its executable.
        auto directory = Platform::executableDirectory();
        if (directory.empty())
            return;
        GChars extensions(g_build_filename(directory.c_str(), extensionsDirectoryName, nullptr));
        if (g_file_test(extensions.get(), G_FILE_TEST_IS_DIR))
            webkit_web_context_set_web_process_extensions_directory(context, extensions.get());
        else
            g_warning("no directory %s: the pages' console messages will not be reported", extensions.get());
    }), nullptr);

    m_display = GRef<WPEDisplay>::adopt(createDisplay());
    if (!m_display)
        return false;
    // No appearance asked for yet: the one the pages see is the system's, as in Safari.
    followSystemAppearance();
    m_transport = std::make_shared<Transport>(input, output, [this](std::string&& payload) {
        handleFrame(std::move(payload));
    }, [this](const char* error) {
        handleClosed(error);
    });
    m_transport->start();
    return true;
}

void Host::stop()
{
    auto blocks = std::exchange(m_blocks, { });
    for (auto& [identifier, block] : blocks)
        block->close();
    blocks.clear();
    if (m_transport)
        m_transport->finish();
    m_sessions.clear();
    if (!m_temporaryDirectory.empty())
        removeTree(m_temporaryDirectory.c_str());
}

void Host::handleClosed(const char* error)
{
    if (error)
        g_warning("the protocol stream broke: %s", error);
    if (auto quit = std::exchange(m_quit, nullptr))
        quit(error ? 1 : 0);
}

void Host::handleFrame(std::string&& payload)
{
    if (!m_quit)
        return;
    auto frame = Json::parse(payload);
    if (!frame.isObject()) {
        handleClosed("a frame is not a JSON object");
        return;
    }
    if (frame.get("hello")) {
#if defined(__APPLE__)
        m_transport->send("{\"welcome\":{\"ops\":[\"browser\",\"dom\",\"console\",\"network\",\"present\"]}}");
#else
        m_transport->send("{\"welcome\":{\"ops\":[\"browser\",\"dom\",\"console\",\"network\"]}}");
#endif
        return;
    }
    for (const char* channel : { "browser", "fork" }) {
        auto body = frame.get(channel);
        if (!body)
            continue;
        auto id = frame.get("id");
        if (!id.isNumber() || id.toNumber() < 0 || id.toNumber() != std::floor(id.toNumber())) {
            handleClosed("a request has no id");
            return;
        }
        auto reply = std::make_shared<Responder>(m_transport, channel, id.serialize());
        if (channel[0] == 'f')
            reply->unsupported("fork: this host has no sessions to fork");
        else if (!body.isObject())
            reply->invalid("a browser op is a JSON object");
        else
            dispatch(body, reply);
        return;
    }
    // Anything else is a member this host does not know, which is ignored.
}

static std::optional<uint64_t> blockIdentifier(const Json& operation)
{
    auto block = operation.numberMember("block");
    if (!block || *block < 0 || *block != std::floor(*block) || *block > 9007199254740992.0)
        return std::nullopt;
    return static_cast<uint64_t>(*block);
}

void Host::dispatch(const Json& operation, const Reply& reply)
{
    auto name = operation.stringMember("op");
    if (!name) {
        reply->invalid("`op` names the operation");
        return;
    }
    AGENT_HOST_DEBUG("op %s", name->c_str());
    if (*name == "open") {
        open(operation, reply);
        return;
    }
    if (*name == "blocks") {
        listBlocks(reply);
        return;
    }
    if (*name == "clipboard") {
        clipboard(operation, reply);
        return;
    }
    if (*name == "clear") {
        clearProfile(operation, reply);
        return;
    }
    auto identifier = blockIdentifier(operation);
    if (!identifier) {
        reply->invalid("`block` names the browser block");
        return;
    }
    auto found = m_blocks.find(*identifier);
    if (found == m_blocks.end()) {
        reply->notFound("no browser block " + std::to_string(*identifier));
        return;
    }
    if (*name == "close" || *name == "release") {
        closeBlock(*identifier, reply);
        return;
    }
    auto block = found->second;
    if (!block->handle(*name, operation, reply))
        reply->invalid("unknown web call " + Json::quote(*name));
}

// A view size member: CSS pixels, 1 to 16384. Returns false after refusing the request.
static bool readDimension(const Json& operation, const char* name, unsigned fallback, unsigned& value, const Reply& reply)
{
    auto member = operation.get(name);
    if (!member || member.isNull()) {
        value = fallback;
        return true;
    }
    double number = member.isNumber() ? std::round(member.toNumber()) : 0;
    if (!(number >= 1 && number <= 16384)) {
        reply->invalid(std::string("`") + name + "` must be 1 to 16384 CSS px");
        return false;
    }
    value = static_cast<unsigned>(number);
    return true;
}

bool readScale(const Json& operation, double& scale, const Reply& reply)
{
    auto member = operation.get("scale");
    if (!member || member.isNull())
        return true;
    double number = member.isNumber() ? member.toNumber() : 0;
    if (!(number >= 0.25 && number <= 8)) {
        reply->invalid("`scale` must be 0.25 to 8 device px per CSS px");
        return false;
    }
    scale = number;
    return true;
}

void Host::open(const Json& operation, const Reply& reply)
{
    std::string url = "about:blank";
    if (auto member = operation.get("url"); member && !member.isNull()) {
        if (!member.isString()) {
            reply->invalid("open: `url` must be a string");
            return;
        }
        if (auto text = member.toString(); !text.empty())
            url = std::move(text);
    }
    unsigned width, height;
    if (!readDimension(operation, "width", 1280, width, reply) || !readDimension(operation, "height", 800, height, reply))
        return;
    double scale = 1;
    if (!readScale(operation, scale, reply))
        return;

    // A profile names a website data store; null asks for one of the block's own, kept nowhere.
    std::optional<std::string> profile = "agent";
    if (auto member = operation.get("profile")) {
        if (member.isNull())
            profile = std::nullopt;
        else {
            auto name = member.isString() ? member.toString() : std::string();
            // The name becomes a directory.
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos || name.find('\0') != std::string::npos) {
                reply->invalid("open: `profile` must be a name or null");
                return;
            }
            profile = std::move(name);
        }
    }

    auto session = sessionForProfile(profile);
    if (!session) {
        reply->failed("open: the profile's website data store could not be made");
        return;
    }
    uint64_t identifier = ++m_lastBlock;
    auto block = std::make_shared<Block>(*this, identifier, width, height, scale, std::move(session));
    m_blocks.emplace(identifier, block);
    block->open(url);

    auto answer = Json::object();
    answer.setNumber("block", static_cast<double>(identifier));
    answer.setString("url", url);
    reply->ok(answer);
}

// Website data stores. None of them is one of the engine's ephemeral sessions: those are its private browsing
// mode, which a page can tell from an ordinary browser's, and in which the engine reports no console message.
// Data that is not to be kept goes to a directory of this process's, removed when it ends.

static const char temporaryPrefix[] = "WPEAgentHost-";

const std::string& Host::temporaryDirectory()
{
    if (!m_temporaryDirectory.empty())
        return m_temporaryDirectory;

    // What hosts that did not get to clean up left behind goes first.
    if (auto* directory = g_dir_open(g_get_tmp_dir(), 0, nullptr)) {
        while (const char* name = g_dir_read_name(directory)) {
            if (!g_str_has_prefix(name, temporaryPrefix))
                continue;
            char* end = nullptr;
            auto owner = g_ascii_strtoull(name + strlen(temporaryPrefix), &end, 10);
            if (!owner || !end || *end != '-' || Platform::processExists(static_cast<unsigned long>(owner)))
                continue;
            GChars stale(g_build_filename(g_get_tmp_dir(), name, nullptr));
            removeTree(stale.get());
        }
        g_dir_close(directory);
    }

    GChars pattern(g_strdup_printf("%s%lu-XXXXXX", temporaryPrefix, Platform::processIdentifier()));
    GChars path(g_build_filename(g_get_tmp_dir(), pattern.get(), nullptr));
    if (g_mkdtemp_full(path.get(), 0700))
        m_temporaryDirectory = path.get();
    else
        g_warning("cannot create a directory in %s", g_get_tmp_dir());
    return m_temporaryDirectory;
}

GRef<WebKitNetworkSession> Host::sessionInDirectory(const std::string& directory)
{
    GChars data(g_build_filename(directory.c_str(), "data", nullptr));
    GChars cache(g_build_filename(directory.c_str(), "cache", nullptr));
    GChars cookies(g_build_filename(directory.c_str(), "cookies.sqlite", nullptr));
    if (g_mkdir_with_parents(directory.c_str(), 0700) < 0) {
        g_warning("cannot create %s", directory.c_str());
        return { };
    }
    auto session = GRef<WebKitNetworkSession>::adopt(webkit_network_session_new(data.get(), cache.get()));
    if (session)
        webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(session.get()), cookies.get(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    return session;
}

GRef<WebKitNetworkSession> Host::sessionForProfile(const std::optional<std::string>& profile)
{
    if (!profile) {
        // A store of the block's own.
        auto& base = temporaryDirectory();
        if (base.empty())
            return { };
        GChars directory(g_build_filename(base.c_str(), "private", std::to_string(++m_lastPrivateStore).c_str(), nullptr));
        return sessionInDirectory(directory.get());
    }
    if (auto found = m_sessions.find(*profile); found != m_sessions.end())
        return found->second;

    auto& base = m_dataDirectory.empty() ? temporaryDirectory() : m_dataDirectory;
    if (base.empty())
        return { };
    GChars directory(m_dataDirectory.empty() ? g_build_filename(base.c_str(), "profiles", profile->c_str(), nullptr) : g_build_filename(base.c_str(), profile->c_str(), nullptr));
    auto session = sessionInDirectory(directory.get());
    if (session)
        m_sessions.emplace(*profile, session);
    return session;
}

void Host::closeBlock(uint64_t identifier, const Reply& reply)
{
    auto found = m_blocks.find(identifier);
    if (found != m_blocks.end()) {
        auto block = std::move(found->second);
        m_blocks.erase(found);
        block->close();
    }
    reply->ok();
}

void Host::listBlocks(const Reply& reply)
{
    auto blocks = Json::array();
    for (auto& [identifier, block] : m_blocks) {
        auto entry = Json::object();
        entry.setNumber("block", static_cast<double>(identifier));
        entry.setString("url", block->url());
        entry.setString("title", block->title());
        entry.setNull("owner");
        entry.setBoolean("shown", block->presented());
        entry.setBoolean("driven", true);
        blocks.append(entry);
    }
    auto answer = Json::object();
    answer.set("blocks", blocks);
    reply->ok(answer);
}

std::shared_ptr<PresentClient> Host::presentClient(const Json& target, const Reply& reply)
{
    if (!target.isObject()) {
        reply->invalid("present: `to` is a target (`{\"mach\":NAME}`) or null");
        return nullptr;
    }
    if (target.get("hwnd") || target.get("socket")) {
        reply->unsupported("present: this host presents to Mach services only (macOS)");
        return nullptr;
    }
    auto service = target.stringMember("mach");
    if (!service || service->empty()) {
        reply->invalid("present: `to` names a Mach service (`{\"mach\":NAME}`)");
        return nullptr;
    }
#if !defined(__APPLE__)
    reply->unsupported("present: Mach services are macOS's");
    return nullptr;
#else
    if (auto found = m_presentClients.find(*service); found != m_presentClients.end() && found->second->alive())
        return found->second;

    std::string error;
    PresentClient::Handlers handlers;
    handlers.released = [this](uint32_t block, uint32_t buffer) {
        if (auto found = m_blocks.find(block); found != m_blocks.end())
            found->second->presentedBufferReleased(buffer);
    };
    handlers.ticked = [] {
        displayTicked();
    };
    handlers.gone = [this, service = *service] {
        presentClientGone(service);
    };
    auto client = PresentClient::connect(*service, std::move(handlers), error);
    if (!client) {
        reply->failed("present: " + error);
        return nullptr;
    }
    m_presentClients[*service] = client;
    return client;
#endif
}

// Erases what a profile's websites keep (cookies, storage, caches). Its open blocks stay open, without it.
void Host::clearProfile(const Json& operation, const Reply& reply)
{
    auto profile = operation.get("profile");
    auto name = profile && profile.isString() ? profile.toString() : std::string();
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos || name.find('\0') != std::string::npos) {
        reply->invalid("clear: `profile` names a profile (a store of a block's own goes with its block)");
        return;
    }
    // A profile not opened in this run may still have data in the data directory.
    auto session = sessionForProfile(name);
    if (!session) {
        reply->failed("clear: the profile's website data store could not be made");
        return;
    }
    auto* manager = webkit_network_session_get_website_data_manager(session.get());
    webkit_website_data_manager_clear(manager, WEBKIT_WEBSITE_DATA_ALL, 0, nullptr, asyncReady, asyncData([reply, session](GObject* source, GAsyncResult* result) {
        GError* error = nullptr;
        if (webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error))
            reply->ok();
        else
            reply->failed("clear: " + takeError(error, "the website data could not be erased"));
    }));
}

// A client that died, or that a message could not reach: its blocks are presented no more.
void Host::presentClientGone(const std::string& service)
{
    auto found = m_presentClients.find(service);
    if (found == m_presentClients.end())
        return;
    auto client = found->second;
    if (client->alive())
        return;
    m_presentClients.erase(found);
    for (auto& [identifier, block] : m_blocks)
        block->presentClientGone(*client);
}

// The clipboard is the display's: what pages of this host copy and paste, not the desktop's.
void Host::clipboard(const Json& operation, const Reply& reply)
{
    auto* clipboard = wpe_display_get_clipboard(m_display.get());
    auto text = operation.get("text");
    if (text.isString()) {
        auto* content = wpe_clipboard_content_new();
        wpe_clipboard_content_set_text(content, text.toString().c_str());
        wpe_clipboard_set_content(clipboard, content);
        wpe_clipboard_content_unref(content);
        reply->ok();
        return;
    }
    if (text && !text.isNull()) {
        reply->invalid("clipboard: `text` must be a string");
        return;
    }
    auto answer = Json::object();
    gsize size = 0;
    GChars held(wpe_clipboard_read_text(clipboard, "text/plain", &size));
    if (held)
        answer.setString("text", std::string_view(held.get(), size));
    reply->ok(answer);
}

void Host::setDarkAppearance(bool dark)
{
    GError* error = nullptr;
    if (!wpe_settings_set_boolean(wpe_display_get_settings(m_display.get()), WPE_SETTING_DARK_MODE, dark, WPE_SETTINGS_SOURCE_APPLICATION, &error)) {
        g_warning("the appearance did not change: %s", error ? error->message : "no reason given");
        g_clear_error(&error);
    }
}

void Host::followSystemAppearance()
{
#if defined(__APPLE__)
    // Safari takes the Mac's appearance from the global AppleInterfaceStyle preference: the string "Dark" in
    // dark mode, unset in light mode.
    CFPropertyListRef style = CFPreferencesCopyAppValue(CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
    bool dark = style && CFGetTypeID(style) == CFStringGetTypeID() && CFEqual(style, CFSTR("Dark"));
    if (style)
        CFRelease(style);
    setDarkAppearance(dark);
#else
    // No system appearance to follow: the engine's default, light.
    setDarkAppearance(false);
#endif
}

} // namespace AgentHost
