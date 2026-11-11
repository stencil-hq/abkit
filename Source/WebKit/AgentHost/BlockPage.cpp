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

#include <cstring>
#include <libsoup/soup.h>

namespace AgentHost {

// What a page asks of the person in front of it, and what that person would
// do in its browser: dialogs, file choosers, editing commands, cookies; and
// reading the page natively.

// Dialogs.

void Block::setDialogPolicy(const Json& operation, const Reply& reply)
{
    auto policy = operation.stringMember("policy").value_or("");
    if (policy == "default")
        m_dialogPolicy = DialogPolicy::Default;
    else if (policy == "accept")
        m_dialogPolicy = DialogPolicy::Accept;
    else if (policy == "dismiss")
        m_dialogPolicy = DialogPolicy::Dismiss;
    else {
        reply->invalid("dialogs: `policy` is accept, dismiss or default");
        return;
    }
    reply->ok();
}

void Block::finishDialog(WebKitScriptDialog* dialog, bool accept, const std::optional<std::string>& text)
{
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        webkit_script_dialog_confirm_set_confirmed(dialog, accept);
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        // Accepted without a text, a prompt gives its default one; dismissed, none.
        if (accept)
            webkit_script_dialog_prompt_set_text(dialog, text ? text->c_str() : webkit_script_dialog_prompt_get_default_text(dialog));
        break;
    }
}

void Block::releaseHeldDialog(bool accept, const std::optional<std::string>& text)
{
    auto* dialog = std::exchange(m_heldDialog, nullptr);
    if (!dialog)
        return;
    finishDialog(dialog, accept, text);
    webkit_script_dialog_close(dialog);
    webkit_script_dialog_unref(dialog);
}

// No dialog is ever shown: each is answered as the policy says, or held for `dialog`, and reported.
bool Block::scriptDialog(WebKitScriptDialog* dialog)
{
    const char* kind = "alert";
    bool asks = false;
    switch (webkit_script_dialog_get_dialog_type(dialog)) {
    case WEBKIT_SCRIPT_DIALOG_ALERT:
        break;
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
        kind = "confirm";
        asks = true;
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        kind = "prompt";
        asks = true;
        break;
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        kind = "beforeunload";
        break;
    }
    // A dialog still held (by another process of the page) is dismissed, as the engine would have.
    releaseHeldDialog(false, std::nullopt);

    bool hold = m_dialogPolicy == DialogPolicy::Default && asks;
    bool accept = m_dialogPolicy != DialogPolicy::Dismiss;
    auto event = eventOfType("dialog");
    event.setString("kind", kind);
    event.setString("message", stringOrEmpty(webkit_script_dialog_get_message(dialog)));
    const char* defaultText = !strcmp(kind, "prompt") ? webkit_script_dialog_prompt_get_default_text(dialog) : nullptr;
    if (defaultText)
        event.setString("default", defaultText);
    else
        event.setNull("default");
    event.setString("url", url());
    if (hold)
        event.setNull("handled");
    else
        event.setString("handled", accept ? "accepted" : "dismissed");
    record(std::move(event));

    if (hold)
        m_heldDialog = webkit_script_dialog_ref(dialog);
    else
        finishDialog(dialog, accept, std::nullopt);
    return true;
}

void Block::answerDialog(const Json& operation, const Reply& reply)
{
    auto accept = operation.booleanMember("accept");
    if (!accept) {
        reply->invalid("`accept` must be true or false");
        return;
    }
    std::optional<std::string> text;
    if (auto member = operation.get("text"); member && !member.isNull()) {
        if (!member.isString()) {
            reply->invalid("`text` must be a string");
            return;
        }
        text = member.toString();
    }
    if (!m_heldDialog) {
        reply->failed("no dialog is open");
        return;
    }
    releaseHeldDialog(*accept, text);
    reply->ok();
}

// File choosers.

void Block::answerChooser(WebKitFileChooserRequest* request, const std::optional<std::vector<std::string>>& paths)
{
    if (!paths) {
        webkit_file_chooser_request_cancel(request);
        return;
    }
    std::vector<const char*> files;
    for (auto& path : *paths)
        files.push_back(path.c_str());
    files.push_back(nullptr);
    webkit_file_chooser_request_select_files(request, files.data());
}

// No chooser is ever shown: each gets the files preset for it, or is held for `files`, and reported.
bool Block::runFileChooser(WebKitFileChooserRequest* request)
{
    if (auto earlier = std::exchange(m_heldChooser, { }))
        answerChooser(earlier.get(), std::nullopt);
    auto preset = std::exchange(m_presetFiles, std::nullopt);
    auto event = eventOfType("chooser");
    event.setBoolean("multiple", webkit_file_chooser_request_get_select_multiple(request));
    // The engine does not say whether the input takes directories.
    event.setBoolean("directories", false);
    event.setBoolean("handled", !!preset);
    record(std::move(event));
    if (preset)
        answerChooser(request, preset);
    else
        m_heldChooser = GRef<WebKitFileChooserRequest>::retain(request);
    return true;
}

void Block::setFiles(const Json& operation, const Reply& reply)
{
    std::optional<std::vector<std::string>> paths;
    if (auto member = operation.get("paths"); member && !member.isNull()) {
        if (!member.isArray()) {
            reply->invalid("files: `paths` must be an array or null");
            return;
        }
        paths.emplace();
        for (unsigned i = 0; i < member.length(); i++) {
            auto path = member.at(i);
            if (!path.isString()) {
                reply->invalid("files: `paths` holds strings");
                return;
            }
            paths->push_back(path.toString());
        }
    }
    // A held chooser is answered (null cancels it); with none held, the files wait for the next one (null: no files wait).
    if (auto chooser = std::exchange(m_heldChooser, { }))
        answerChooser(chooser.get(), paths);
    else
        m_presetFiles = std::move(paths);
    reply->ok();
}

// Editing commands. The clipboard they use is the display's (see Host::clipboard).

void Block::edit(const Json& operation, const Reply& reply)
{
    static const struct {
        const char* action;
        const char* command;
    } commands[] = {
        { "copy", WEBKIT_EDITING_COMMAND_COPY },
        { "cut", WEBKIT_EDITING_COMMAND_CUT },
        { "paste", WEBKIT_EDITING_COMMAND_PASTE },
        { "selectAll", WEBKIT_EDITING_COMMAND_SELECT_ALL },
        { "undo", WEBKIT_EDITING_COMMAND_UNDO },
        { "redo", WEBKIT_EDITING_COMMAND_REDO },
    };
    auto action = operation.get("action");
    auto name = action.isString() ? action.toString() : std::string();
    for (auto& command : commands) {
        if (name != command.action)
            continue;
        webkit_web_view_execute_editing_command(m_view.get(), command.command);
        // Answered once the page took the command.
        runPatiently("", 500, [reply] {
            reply->ok();
        });
        return;
    }
    reply->invalid("edit: unknown action " + (action.isString() ? Json::quote(name) : action.serialize()));
}

// Cookies, of the block's website data store.

void Block::allCookies(CookiesHandler&& then)
{
    auto* manager = webkit_network_session_get_cookie_manager(m_session.get());
    webkit_cookie_manager_get_all_cookies(manager, nullptr, asyncReady, asyncData([then = std::move(then)](GObject* manager, GAsyncResult* result) {
        GError* error = nullptr;
        GList* cookies = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(manager), result, &error);
        then(cookies, error);
        g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
        g_clear_error(&error);
    }));
}

void Block::cookies(const Json&, const Reply& reply)
{
    allCookies([reply](GList* cookies, GError* error) {
        if (error) {
            reply->failed(std::string("cookies: ") + error->message);
            return;
        }
        auto list = Json::array();
        for (GList* link = cookies; link; link = link->next) {
            auto* cookie = static_cast<SoupCookie*>(link->data);
            auto entry = Json::object();
            entry.setString("name", stringOrEmpty(soup_cookie_get_name(cookie)));
            entry.setString("value", stringOrEmpty(soup_cookie_get_value(cookie)));
            entry.setString("domain", stringOrEmpty(soup_cookie_get_domain(cookie)));
            entry.setString("path", stringOrEmpty(soup_cookie_get_path(cookie)));
            if (auto* expires = soup_cookie_get_expires(cookie))
                entry.setNumber("expires", static_cast<double>(g_date_time_to_unix(expires)));
            else
                entry.setNull("expires");
            entry.setBoolean("httpOnly", soup_cookie_get_http_only(cookie));
            entry.setBoolean("secure", soup_cookie_get_secure(cookie));
            switch (soup_cookie_get_same_site_policy(cookie)) {
            case SOUP_SAME_SITE_POLICY_NONE:
                entry.setString("sameSite", "None");
                break;
            case SOUP_SAME_SITE_POLICY_LAX:
                entry.setString("sameSite", "Lax");
                break;
            case SOUP_SAME_SITE_POLICY_STRICT:
                entry.setString("sameSite", "Strict");
                break;
            }
            list.append(entry);
        }
        auto answer = Json::object();
        answer.set("cookies", list);
        reply->ok(answer);
    });
}

void Block::setCookie(const Json& operation, const Reply& reply)
{
    auto cookie = operation.get("cookie");
    if (!cookie.isObject()) {
        reply->invalid("setCookie: `cookie` is missing");
        return;
    }
    auto name = cookie.stringMember("name");
    auto value = cookie.stringMember("value");
    auto domain = cookie.stringMember("domain");
    for (auto [member, present] : { std::pair { "name", !!name }, std::pair { "value", !!value }, std::pair { "domain", !!domain } }) {
        if (!present) {
            reply->invalid(std::string("`") + member + "` must be a string");
            return;
        }
    }
    auto path = cookie.get("path");
    if (path && !path.isNull() && !path.isString()) {
        reply->invalid("`path` must be a string");
        return;
    }
    auto expires = cookie.get("expires");
    if (expires && !expires.isNull() && !expires.isNumber()) {
        reply->invalid("`expires` must be a number");
        return;
    }
    if (name->empty() || domain->empty()) {
        reply->failed("the cookie " + Json::quote(*name) + " of " + Json::quote(*domain) + " is not valid");
        return;
    }

    auto* soupCookie = soup_cookie_new(name->c_str(), value->c_str(), domain->c_str(), path.isString() && !path.toString().empty() ? path.toString().c_str() : "/", -1);
    if (!soupCookie) {
        reply->failed("the cookie " + Json::quote(*name) + " of " + Json::quote(*domain) + " is not valid");
        return;
    }
    if (expires.isNumber()) {
        if (auto* date = g_date_time_new_from_unix_utc(static_cast<gint64>(expires.toNumber()))) {
            soup_cookie_set_expires(soupCookie, date);
            g_date_time_unref(date);
        }
    }
    soup_cookie_set_http_only(soupCookie, cookie.booleanMember("httpOnly").value_or(false));
    soup_cookie_set_secure(soupCookie, cookie.booleanMember("secure").value_or(false));
    if (auto sameSite = cookie.stringMember("sameSite")) {
        GChars lowered(g_ascii_strdown(sameSite->c_str(), -1));
        if (!strcmp(lowered.get(), "strict"))
            soup_cookie_set_same_site_policy(soupCookie, SOUP_SAME_SITE_POLICY_STRICT);
        else if (!strcmp(lowered.get(), "lax"))
            soup_cookie_set_same_site_policy(soupCookie, SOUP_SAME_SITE_POLICY_LAX);
        else if (!strcmp(lowered.get(), "none"))
            soup_cookie_set_same_site_policy(soupCookie, SOUP_SAME_SITE_POLICY_NONE);
    }
    auto* manager = webkit_network_session_get_cookie_manager(m_session.get());
    webkit_cookie_manager_add_cookie(manager, soupCookie, nullptr, asyncReady, asyncData([reply](GObject* manager, GAsyncResult* result) {
        GError* error = nullptr;
        if (webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(manager), result, &error))
            reply->ok();
        else
            reply->failed("set cookie: " + takeError(error, "refused"));
    }));
    soup_cookie_free(soupCookie);
}

void Block::deleteCookie(const Json& operation, const Reply& reply)
{
    auto name = operation.stringMember("name");
    auto domain = operation.stringMember("domain");
    if (!name || !domain) {
        reply->invalid(std::string("`") + (name ? "domain" : "name") + "` must be a string");
        return;
    }
    auto pathMember = operation.get("path");
    if (pathMember && !pathMember.isNull() && !pathMember.isString()) {
        reply->invalid("`path` must be a string");
        return;
    }
    std::string path = pathMember.isString() ? pathMember.toString() : "/";

    // The stored cookie of that name, domain and path, if any: a missing one is no error.
    allCookies([weakThis = weak_from_this(), reply, name = *name, domain = *domain, path](GList* cookies, GError* error) {
        auto block = weakThis.lock();
        if (!block || block->m_closed)
            return;
        if (error) {
            reply->failed(std::string("cookies: ") + error->message);
            return;
        }
        auto bare = [](const char* domain) {
            return domain && *domain == '.' ? domain + 1 : (domain ? domain : "");
        };
        for (GList* link = cookies; link; link = link->next) {
            auto* cookie = static_cast<SoupCookie*>(link->data);
            if (name != stringOrEmpty(soup_cookie_get_name(cookie)) || path != stringOrEmpty(soup_cookie_get_path(cookie))
                || g_ascii_strcasecmp(bare(domain.c_str()), bare(soup_cookie_get_domain(cookie))))
                continue;
            auto* manager = webkit_network_session_get_cookie_manager(block->m_session.get());
            webkit_cookie_manager_delete_cookie(manager, cookie, nullptr, asyncReady, asyncData([reply](GObject* manager, GAsyncResult* result) {
                GError* error = nullptr;
                if (webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(manager), result, &error) || !error)
                    reply->ok();
                else
                    reply->failed("delete cookie: " + takeError(error, "refused"));
            }));
            return;
        }
        reply->ok();
    });
}

// Native DOM reads: the request goes to the engine as it came, less the two members that are the protocol's,
// and the engine's reply is the answer as it is (an error reply too: the client reads it).
void Block::query(const Json& operation, const Reply& reply)
{
    Json request = operation;
    request.remove("op");
    request.remove("block");
    reply->setUnansweredMessage("dom: the page went away");
    webkit_web_view_run_interaction_query(m_view.get(), request.serialize().c_str(), nullptr, asyncReady, asyncData([reply](GObject* view, GAsyncResult* result) {
        GError* error = nullptr;
        char* text = webkit_web_view_run_interaction_query_finish(WEBKIT_WEB_VIEW(view), result, &error);
        if (!text) {
            reply->failed("dom: " + takeError(error, "the engine gave no reply"));
            return;
        }
        auto answer = takeString(text);
        if (answer.empty() || !g_utf8_validate(answer.data(), static_cast<gssize>(answer.size()), nullptr)) {
            reply->failed("dom: the engine's reply is not JSON");
            return;
        }
        reply->ok(answer);
    }));
}

} // namespace AgentHost
