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

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AgentHost {

// What the protocol sets on a block. Settings are the view's (its settings
// object, its content manager) or kept in the block and applied as the page
// asks, so they hold for the block's life, whatever it loads and whichever
// web process shows it.

#if defined(__APPLE__)
// What Safari on a Mac sends. The engine's own default names a version no Safari has.
static const char safariUserAgent[] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.2 Safari/605.1.15";
#endif

// What a block is to its pages before any setting: on a Mac, Safari.
void Block::applyDefaultIdentity()
{
#if defined(__APPLE__)
    setFeature("SafariNamespace", true);
    // The fonts Safari gives the generic families, and text that names no font it has. The engine's
    // defaults are fontconfig's answers, which on a Mac are fonts no Mac browser uses for them.
    webkit_settings_set_default_font_family(m_settings.get(), "Times");
    webkit_settings_set_serif_font_family(m_settings.get(), "Times");
    webkit_settings_set_sans_serif_font_family(m_settings.get(), "Helvetica");
    webkit_settings_set_monospace_font_family(m_settings.get(), "Courier");
    webkit_settings_set_cursive_font_family(m_settings.get(), "Apple Chancery");
    webkit_settings_set_fantasy_font_family(m_settings.get(), "Papyrus");
    webkit_settings_set_pictograph_font_family(m_settings.get(), "Apple Color Emoji");
#endif
    applyDefaultUserAgent();
    applyIdentity();
}

void Block::applyDefaultUserAgent()
{
#if defined(__APPLE__)
    webkit_settings_set_user_agent(m_settings.get(), safariUserAgent);
#else
    // The engine's own.
    webkit_settings_set_user_agent(m_settings.get(), nullptr);
#endif
}

void Block::setAgent(const Json& operation, const Reply& reply)
{
    auto value = operation.get("value");
    if (value.isString())
        webkit_settings_set_user_agent(m_settings.get(), value.toString().c_str());
    else if (!value || value.isNull())
        applyDefaultUserAgent();
    else {
        reply->invalid("`value` must be a string");
        return;
    }
    reply->ok();
}

void Block::setAppearance(const Json& operation, const Reply& reply)
{
    auto value = operation.get("value");
    if (value.isString() && value.toString() == "dark")
        m_host.setDarkAppearance(true);
    else if (value.isString() && value.toString() == "light")
        m_host.setDarkAppearance(false);
    else if (value && !value.isNull()) {
        reply->invalid("appearance: unknown value " + (value.isString() ? Json::quote(value.toString()) : value.serialize()) + " (dark, light, null)");
        return;
    } else
        m_host.followSystemAppearance();
    // Answered once the page drew in its new colours.
    runPatiently("await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));", 500, [reply] {
        reply->ok();
    });
}

// A member that is a whole number in [minimum, maximum].
static std::optional<double> integerMember(const Json& object, const char* name, double minimum, double maximum)
{
    auto number = object.numberMember(name);
    if (!number || *number != std::floor(*number) || *number < minimum || *number > maximum)
        return std::nullopt;
    return number;
}

// What a page can read of the screen and of its window. A view on no screen would say that its screen is its
// viewport and its window has no frame, which no browser a person uses says; so unless `identity` gave one, every
// block has a profile made for its viewport: a window with browser chrome above the page, on the smallest common
// screen it fits, with the desktop's bar taken off the available area.
void Block::applyIdentity()
{
    // The window's place on the screen, and the height of the chrome above the page.
    constexpr int windowX = 80;
    constexpr int windowY = 60;
    constexpr unsigned chromeHeight = 87;
#if defined(__APPLE__)
    // A menu bar at the top.
    constexpr unsigned barTop = 25;
    constexpr unsigned barBottom = 0;
#else
    // A task bar at the bottom.
    constexpr unsigned barTop = 0;
    constexpr unsigned barBottom = 40;
#endif
    WindowProfile window = m_pinnedWindow.value_or(WindowProfile { windowX, windowY, m_width, m_height + chromeHeight });

    ScreenProfile screen;
    if (m_pinnedScreen)
        screen = *m_pinnedScreen;
    else {
        // The first common screen whose available area holds the window; else one just as large as it takes.
        static constexpr std::pair<unsigned, unsigned> sizes[] = { { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 } };
        unsigned right = static_cast<unsigned>(std::max<long>(0, static_cast<long>(window.x) + window.width));
        unsigned bottom = static_cast<unsigned>(std::max<long>(0, static_cast<long>(window.y) + window.height)) + barBottom;
        std::pair<unsigned, unsigned> size { std::max(right, 1u), std::max(bottom, barTop + 1) };
        for (auto& candidate : sizes) {
            if (right <= candidate.first && bottom <= candidate.second) {
                size = candidate;
                break;
            }
        }
        screen = { size.first, size.second, 0, static_cast<int>(barTop), size.first, size.second - barTop - barBottom };
    }
    webkit_settings_set_screen_override(m_settings.get(), screen.width, screen.height, screen.availLeft, screen.availTop, screen.availWidth, screen.availHeight);
    webkit_settings_set_window_frame_override(m_settings.get(), window.x, window.y, window.width, window.height);
}

void Block::setIdentity(const Json& operation, const Reply& reply)
{
    // Everything is checked before anything changes.
    auto seed = operation.get("seed");
    if (seed && !seed.isNull() && !seed.isString()) {
        reply->invalid("identity: `seed` must be a string or null");
        return;
    }
    const double most = 1 << 24;
    auto screen = operation.get("screen");
    ScreenProfile screenProfile { };
    if (screen.isObject()) {
        auto width = integerMember(screen, "width", 1, most);
        auto height = integerMember(screen, "height", 1, most);
        auto availLeft = integerMember(screen, "availLeft", -most, most);
        auto availTop = integerMember(screen, "availTop", -most, most);
        auto availWidth = integerMember(screen, "availWidth", 0, most);
        auto availHeight = integerMember(screen, "availHeight", 0, most);
        if (!width || !height || !availLeft || !availTop || !availWidth || !availHeight) {
            reply->invalid("identity: `screen` is {width, height, availLeft, availTop, availWidth, availHeight} in CSS px, or null");
            return;
        }
        screenProfile = { static_cast<unsigned>(*width), static_cast<unsigned>(*height), static_cast<int>(*availLeft), static_cast<int>(*availTop), static_cast<unsigned>(*availWidth), static_cast<unsigned>(*availHeight) };
    } else if (screen && !screen.isNull()) {
        reply->invalid("identity: `screen` is {width, height, availLeft, availTop, availWidth, availHeight} in CSS px, or null");
        return;
    }
    auto window = operation.get("window");
    WindowProfile windowProfile { };
    if (window.isObject()) {
        auto x = integerMember(window, "x", -most, most);
        auto y = integerMember(window, "y", -most, most);
        auto width = integerMember(window, "width", 1, most);
        auto height = integerMember(window, "height", 1, most);
        if (!x || !y || !width || !height) {
            reply->invalid("identity: `window` is {x, y, width, height} in CSS px, or null");
            return;
        }
        windowProfile = { static_cast<int>(*x), static_cast<int>(*y), static_cast<unsigned>(*width), static_cast<unsigned>(*height) };
    } else if (window && !window.isNull()) {
        reply->invalid("identity: `window` is {x, y, width, height} in CSS px, or null");
        return;
    }

    // A member left out stays as it is. A seed of null is no seed; a screen or window of null is the block's own again.
    if (seed.isString())
        webkit_settings_set_fingerprinting_noise_seed(m_settings.get(), seed.toString().c_str());
    else if (seed.isNull())
        webkit_settings_set_fingerprinting_noise_seed(m_settings.get(), nullptr);
    if (screen.isObject())
        m_pinnedScreen = screenProfile;
    else if (screen.isNull())
        m_pinnedScreen = std::nullopt;
    if (window.isObject())
        m_pinnedWindow = windowProfile;
    else if (window.isNull())
        m_pinnedWindow = std::nullopt;
    applyIdentity();
    reply->ok();
}

void Block::setInsecure(const Json& operation, const Reply& reply)
{
    auto value = operation.booleanMember("value");
    if (!value) {
        reply->invalid("`value` must be true or false");
        return;
    }
    m_insecure = *value;
    reply->ok();
}

// What Chromium's --allow-file-access-from-files allows: a file: document reads other files (fetch, XHR, frames,
// canvas pixels) as if they shared its origin. Documents loaded afterwards see the change.
void Block::setFileAccess(const Json& operation, const Reply& reply)
{
    auto value = operation.booleanMember("value");
    if (!value) {
        reply->invalid("`value` must be true or false");
        return;
    }
    webkit_settings_set_allow_file_access_from_file_urls(m_settings.get(), *value);
    webkit_settings_set_allow_universal_access_from_file_urls(m_settings.get(), *value);
    reply->ok();
}

void Block::setCredentials(const Json& operation, const Reply& reply)
{
    auto username = operation.get("username");
    auto password = operation.get("password");
    bool hasUsername = username && !username.isNull();
    bool hasPassword = password && !password.isNull();
    if ((hasUsername && !username.isString()) || (hasPassword && !password.isString()) || hasUsername != hasPassword) {
        reply->invalid("credentials: `username` and `password` are two strings, or both left out");
        return;
    }
    if (hasUsername)
        m_credentials = std::make_pair(username.toString(), password.toString());
    else
        m_credentials = std::nullopt;
    reply->ok();
}

void Block::setDownloads(const Json& operation, const Reply& reply)
{
    auto directory = operation.get("dir");
    if (directory.isString() && !directory.toString().empty())
        m_downloadDirectory = directory.toString();
    else if (!directory || directory.isNull())
        m_downloadDirectory = std::nullopt;
    else {
        reply->invalid("`dir` must be a string");
        return;
    }
    reply->ok();
}

bool Block::authenticate(WebKitAuthenticationRequest* request)
{
    // The credentials answer a challenge that takes a user name and a password, once: a retry means they were
    // wrong. Any other challenge goes on without credentials (the page gets the server's refusal): the engine
    // itself would leave it open, and the load waiting, for a dialog this host never shows.
    bool takesPassword = false;
    switch (webkit_authentication_request_get_scheme(request)) {
    case WEBKIT_AUTHENTICATION_SCHEME_DEFAULT:
    case WEBKIT_AUTHENTICATION_SCHEME_HTTP_BASIC:
    case WEBKIT_AUTHENTICATION_SCHEME_HTTP_DIGEST:
    case WEBKIT_AUTHENTICATION_SCHEME_NTLM:
    case WEBKIT_AUTHENTICATION_SCHEME_NEGOTIATE:
        takesPassword = true;
        break;
    default:
        break;
    }
    if (!m_credentials || !takesPassword || webkit_authentication_request_is_retry(request)) {
        webkit_authentication_request_authenticate(request, nullptr);
        return true;
    }
    auto* credential = webkit_credential_new(m_credentials->first.c_str(), m_credentials->second.c_str(), WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
    webkit_authentication_request_authenticate(request, credential);
    webkit_credential_free(credential);
    return true;
}

// The host of an address (lowercase, without user info, port or IPv6 brackets), if it has one.
static std::optional<std::string> hostOf(const std::string& uri)
{
    auto scheme = uri.find("://");
    if (scheme == std::string::npos)
        return std::nullopt;
    auto authority = uri.substr(scheme + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (auto user = authority.rfind('@'); user != std::string::npos)
        authority = authority.substr(user + 1);
    std::string host;
    if (!authority.empty() && authority[0] == '[')
        host = authority.substr(1, authority.find(']') == std::string::npos ? std::string::npos : authority.find(']') - 1);
    else
        host = authority.substr(0, authority.find(':'));
    if (host.empty())
        return std::nullopt;
    for (auto& character : host)
        character = g_ascii_tolower(character);
    return host;
}

// Whether navigations may go to `host`: exact names, and `*.name` for a name and its subdomains.
static bool hostAllowed(std::string host, const std::vector<std::string>& allowed)
{
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    for (auto name : allowed) {
        for (auto& character : name)
            character = g_ascii_tolower(character);
        if (name.rfind("*.", 0)) {
            if (host == name)
                return true;
            continue;
        }
        auto domain = name.substr(2);
        if (host == domain)
            return true;
        if (host.size() > domain.size() && !host.compare(host.size() - domain.size(), domain.size(), domain) && host[host.size() - domain.size() - 1] == '.')
            return true;
    }
    return false;
}

// The content rule list blocking loads from hosts other than `hosts`: every
// load with a host is blocked, then the allowed hosts' loads are exempted.
static std::string allowRules(const std::vector<std::string>& hosts)
{
    auto rules = Json::array();
    auto rule = [&rules](const std::string& filter, const char* action) {
        auto trigger = Json::object();
        trigger.setString("url-filter", filter);
        auto type = Json::object();
        type.setString("type", action);
        auto entry = Json::object();
        entry.set("trigger", trigger);
        entry.set("action", type);
        rules.append(entry);
    };
    rule("^[^:]+://[^/]", "block");
    // The PDF viewer's own resources use a scheme with no host an allowlist could name.
    rule("^webkit-pdfjs-viewer://", "ignore-previous-rules");
    for (auto host : hosts) {
        for (auto& character : host)
            character = g_ascii_tolower(character);
        bool subdomains = !host.rfind("*.", 0);
        if (subdomains)
            host = host.substr(2);
        std::string escaped;
        for (char character : host) {
            if (strchr(".*+?^$()[]{}|\\/", character))
                escaped += '\\';
            escaped += character;
        }
        rule((subdomains ? "^[^:]+://([^/:]+\\.)?" : "^[^:]+://") + escaped + "[:/]", "ignore-previous-rules");
    }
    return rules.serialize();
}

void Block::setAllowedHosts(const Json& operation, const Reply& reply)
{
    auto hosts = operation.get("hosts");
    if (!hosts || hosts.isNull()) {
        m_allowGeneration++;
        m_allowedHosts = std::nullopt;
        if (auto* filter = std::exchange(m_allowFilter, nullptr)) {
            webkit_user_content_manager_remove_filter(m_contentManager.get(), filter);
            webkit_user_content_filter_unref(filter);
        }
        reply->ok();
        return;
    }
    if (!hosts.isArray()) {
        reply->invalid("allow: `hosts` must be an array or null");
        return;
    }
    std::vector<std::string> names;
    for (unsigned i = 0; i < hosts.length(); i++) {
        auto name = hosts.at(i);
        if (!name.isString()) {
            reply->invalid("allow: `hosts` holds strings");
            return;
        }
        names.push_back(name.toString());
    }

    // Navigations are refused here (and reported); everything else the page loads, by a content filter,
    // in force once it compiled.
    uint64_t generation = ++m_allowGeneration;
    m_allowedHosts = names;
    auto& temporary = m_host.temporaryDirectory();
    if (temporary.empty()) {
        reply->failed("allow: there is no directory to compile the rules in");
        return;
    }
    GChars directory(g_build_filename(temporary.c_str(), "rules", nullptr));
    auto rules = allowRules(names);
    GChars name(g_strdup_printf("allow-%08x", g_str_hash(rules.c_str())));
    auto* store = webkit_user_content_filter_store_new(directory.get());
    auto* bytes = g_bytes_new(rules.data(), rules.size());
    webkit_user_content_filter_store_save(store, name.get(), bytes, nullptr, asyncReady, asyncData([weakThis = weak_from_this(), generation, reply](GObject* store, GAsyncResult* result) {
        GError* error = nullptr;
        auto* filter = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), result, &error);
        g_object_unref(store);
        auto block = weakThis.lock();
        if (!filter) {
            reply->failed("allow: " + takeError(error, "the rules did not compile"));
            return;
        }
        if (!block || block->m_closed || block->m_allowGeneration != generation) {
            webkit_user_content_filter_unref(filter);
            if (block && !block->m_closed)
                reply->ok();
            return;
        }
        if (auto* old = std::exchange(block->m_allowFilter, filter)) {
            webkit_user_content_manager_remove_filter(block->m_contentManager.get(), old);
            webkit_user_content_filter_unref(old);
        }
        webkit_user_content_manager_add_filter(block->m_contentManager.get(), filter);
        reply->ok();
    }));
    g_bytes_unref(bytes);
}

bool Block::decideNavigation(WebKitNavigationPolicyDecision* decision)
{
    if (!m_allowedHosts)
        return false;
    auto* action = webkit_navigation_policy_decision_get_navigation_action(decision);
    auto* request = action ? webkit_navigation_action_get_request(action) : nullptr;
    const char* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (!uri)
        return false;
    auto host = hostOf(uri);
    if (!host || hostAllowed(*host, *m_allowedHosts))
        return false;
    webkit_policy_decision_ignore(WEBKIT_POLICY_DECISION(decision));
    auto event = eventOfType("blocked");
    event.setString("url", uri);
    record(std::move(event));
    return true;
}

bool Block::loadFailedWithTLSErrors(const char* uri, GTlsCertificate* certificate)
{
    if (!m_insecure || !uri || !certificate)
        return false;
    auto host = hostOf(uri);
    if (!host)
        return false;
    // The certificate is accepted for its host, and the address loaded again.
    webkit_network_session_allow_tls_certificate_for_host(m_session.get(), certificate, host->c_str());
    // This load ends without its document.
    m_loadFailed = true;
    m_errorPage = std::nullopt;
    runAfter(0, [weakThis = weak_from_this(), address = std::string(uri)] {
        auto block = weakThis.lock();
        if (block && !block->m_closed)
            webkit_web_view_load_uri(block->m_view.get(), address.c_str());
    });
    return true;
}

} // namespace AgentHost
