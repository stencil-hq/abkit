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

namespace AgentHost {

// The handlers page scripts post to: messages, and the names frames give themselves.
static const char messageHandler[] = "stencil";
static const char frameHandler[] = "stencilFrame";
// The private handler the host's script in the isolated world posts through.
static const char isolatedHostHandler[] = "stencilHost";
// The argument of an evaluated function body holding the call's arguments (JSON text).
static const char argumentsName[] = "__stencilArgs";

// `script-message-received` says neither the world nor the frame a message
// came from. In the isolated world, which pages cannot see into, this script
// runs first in every document: it makes the two handlers post through the
// private one with the frame's address, whether it is the main frame and its
// index path from the top, and it runs the calls the main frame's isolated
// world posts to this frame (__TOKEN__), answering through the same handler.
// (The engine makes the script objects of `webkit`, its handlers and each
// handler anew once they were collected, without what was defined on them:
// `kept`, which the window's message listener holds, keeps them.)
// The page's own world is left as the engine made it, and has the two
// handlers only while the client runs a script of its own there (see
// setPageWorldHandlers()): what is posted there arrives without its frame.
static const char hostScript[] = R"JS((() => {
const handlers = globalThis.webkit && webkit.messageHandlers;
const host = handlers && handlers.__HOST__;
if (!host) return;
const stencil = handlers.stencil;
if (stencil && Object.prototype.hasOwnProperty.call(stencil, 'postMessage')) return;
const post = host.postMessage.bind(host);
const where = () => {
	let path = '';
	try {
		const parts = [];
		for (let w = window; w !== w.parent; w = w.parent) {
			const parent = w.parent;
			let i = 0;
			while (i < parent.length && parent[i] !== w) i++;
			if (i === parent.length) throw 0;
			parts.unshift(i);
		}
		path = parts.join('.');
	} catch (e) { path = '-'; }
	return (window === top ? '1' : '0') + path + '\n' + location.href + '\n';
};
const kept = [webkit, handlers, host];
for (const [name, kind] of [['stencil', 'm'], ['stencilFrame', 'f']]) {
	const handler = handlers[name];
	if (handler) Object.defineProperty(handler, 'postMessage', {
		configurable: true, writable: true,
		value: function (body) { if (typeof body === 'string') post(kind + where() + body); },
	});
	kept.push(handler);
}
const AsyncFunction = (async () => {}).constructor;
const inPage = (body, args) => new Promise((resolve, reject) => {
	const nonce = 'stencil' + Math.random().toString(36).slice(2) + Date.now().toString(36);
	let started = false;
	const listen = (event) => {
		const detail = String(event.detail);
		if (detail === 's') { started = true; return; }
		document.removeEventListener(nonce, listen, true);
		resolve(detail.slice(1));
	};
	document.addEventListener(nonce, listen, true);
	const script = document.createElement('script');
	script.textContent = '(async () => { const say = (d) => document.dispatchEvent(new CustomEvent('
		+ JSON.stringify(nonce) + ', { detail: d })); say(\'s\'); say(\'r\' + await (async function (__ARGS__) {\n'
		+ body + '\n})(' + JSON.stringify(args) + ')); })();';
	(document.head || document.documentElement).appendChild(script);
	script.remove();
	if (!started) {
		document.removeEventListener(nonce, listen, true);
		reject(new Error('the frame\'s content security policy keeps scripts out of its page world'));
	}
});
addEventListener('message', (event) => {
	void kept;
	const data = event.data;
	const call = data !== null && typeof data === 'object' ? data['__TOKEN__'] : undefined;
	if (call === undefined) return;
	event.stopImmediatePropagation();
	(async () => {
		let result;
		try {
			result = call.world === 'page'
				? await inPage(call.body, call.args)
				: await new AsyncFunction('__ARGS__', call.body)(call.args);
		} catch (e) {
			result = 'e' + (e && e.name || 'Error') + ': ' + (e && e.message || String(e));
		}
		post('a' + call.id + '\n' + result);
	})();
}, true);
})();)JS";

// Posts a call (`[path, call]`) from the main frame's isolated world to the frame at `path`.
static const char routeScript[] = R"JS(const [path, call] = JSON.parse(__ARGS__);
let w = window;
for (const i of path) { w = w.frames[i]; if (!w) throw new Error('eval: the frame is gone'); }
w.postMessage({ '__TOKEN__': call }, '*');)JS";

static std::string replaceAll(std::string text, const std::string& what, const std::string& with)
{
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
        text.replace(at, what.size(), with);
    return text;
}

// The text of a script message, when it is a string.
static std::optional<std::string> messageText(JSCValue* value)
{
    if (!value || !jsc_value_is_string(value))
        return std::nullopt;
    auto* bytes = jsc_value_to_string_as_bytes(value);
    if (!bytes)
        return std::nullopt;
    gsize size = 0;
    auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    std::string text(data ? data : "", size);
    g_bytes_unref(bytes);
    return text;
}

void Block::installHostScripts()
{
    auto* manager = m_contentManager.get();
    webkit_user_content_manager_register_script_message_handler(manager, messageHandler, isolatedWorldName);
    webkit_user_content_manager_register_script_message_handler(manager, frameHandler, isolatedWorldName);
    webkit_user_content_manager_register_script_message_handler(manager, isolatedHostHandler, isolatedWorldName);

    g_signal_connect(manager, "script-message-received::stencil", G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer block) {
        if (auto text = messageText(value))
            static_cast<Block*>(block)->bareMessage(*text, false);
    }), this);
    g_signal_connect(manager, "script-message-received::stencilFrame", G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer block) {
        if (auto text = messageText(value))
            static_cast<Block*>(block)->bareMessage(*text, true);
    }), this);
    g_signal_connect(manager, "script-message-received::stencilHost", G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer block) {
        if (auto text = messageText(value))
            static_cast<Block*>(block)->wrappedMessage(*text);
    }), this);

    auto source = replaceAll(replaceAll(replaceAll(hostScript, "__HOST__", isolatedHostHandler), "__TOKEN__", m_host.frameToken()), "__ARGS__", argumentsName);
    auto* script = webkit_user_script_new_for_world(source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, isolatedWorldName, nullptr, nullptr);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
}

// The page's own world gets the two handlers only while a script the client set runs there: a page nothing was
// put into finds no name of the host's.
void Block::setPageWorldHandlers(bool wanted)
{
    if (std::exchange(m_pageWorldHandlers, wanted) == wanted)
        return;
    for (const char* name : { messageHandler, frameHandler }) {
        if (wanted)
            webkit_user_content_manager_register_script_message_handler(m_contentManager.get(), name, nullptr);
        else
            webkit_user_content_manager_unregister_script_message_handler(m_contentManager.get(), name, nullptr);
    }
}

// A message the host's script posted: `m` or `f`, whether the frame is the main one (1 or 0), the frame's path,
// a line break, the frame's address, a line break and the body; or `a`, a frame call's number, a line break and
// its tagged result.
void Block::wrappedMessage(const std::string& text)
{
    if (text.empty())
        return;
    char kind = text[0];
    auto firstBreak = text.find('\n');
    if (firstBreak == std::string::npos)
        return;
    if (kind == 'a') {
        char* end = nullptr;
        uint64_t call = g_ascii_strtoull(text.c_str() + 1, &end, 10);
        if (end != text.c_str() + firstBreak)
            return;
        auto found = m_frameCalls.find(call);
        if (found == m_frameCalls.end())
            return;
        auto reply = std::move(found->second);
        m_frameCalls.erase(found);
        answerEvaluation(reply, text.substr(firstBreak + 1));
        return;
    }
    auto secondBreak = text.find('\n', firstBreak + 1);
    if (secondBreak == std::string::npos || firstBreak < 2)
        return;
    bool main = text[1] == '1';
    std::string path = text.substr(2, firstBreak - 2);
    std::string address = text.substr(firstBreak + 1, secondBreak - firstBreak - 1);
    std::string body = text.substr(secondBreak + 1);

    auto event = Json::object();
    if (kind == 'm') {
        event.setString("type", "message");
        event.setString("body", body);
        event.setString("world", "isolated");
    } else if (kind == 'f') {
        std::optional<std::vector<unsigned>> indices;
        if (path != "-") {
            indices.emplace();
            const char* at = path.c_str();
            while (*at) {
                char* end = nullptr;
                auto index = g_ascii_strtoull(at, &end, 10);
                if (end == at || (*end && *end != '.') || index > G_MAXUINT) {
                    indices = std::nullopt;
                    break;
                }
                indices->push_back(static_cast<unsigned>(index));
                at = *end ? end + 1 : end;
            }
        }
        m_frames[body] = std::move(indices);
        event.setString("type", "frame");
        event.setString("name", body);
    } else
        return;
    event.setBoolean("main", main);
    event.setString("url", address);
    record(std::move(event));
}

// A message posted to a handler of the page's world: its frame is unknown.
void Block::bareMessage(const std::string& body, bool namesFrame)
{
    auto event = Json::object();
    if (namesFrame) {
        m_frames.emplace(body, std::nullopt);
        event.setString("type", "frame");
        event.setString("name", body);
        event.setBoolean("main", false);
    } else {
        event.setString("type", "message");
        event.setString("body", body);
        event.setString("world", "page");
        event.setBoolean("main", true);
    }
    event.setString("url", url());
    record(std::move(event));
}

// The main frame has a new document (a navigation committed), or none (its process died). Frames named
// themselves in the old one. A function still running there never returns: the engine keeps its call open
// for as long as the old document is (it may sit in the back/forward cache), so the host answers for it.
void Block::documentReplaced()
{
    m_frames.clear();
    for (auto& [call, reply] : std::exchange(m_frameCalls, { }))
        reply->failed("eval: the frame's document went away");

    if (m_evaluations.empty())
        return;
    // A call made just before the host heard of the navigation may be running in the new document: those that
    // answer before a round trip made now does (it is behind them in the page's queue) are left alone.
    auto pending = std::make_shared<std::map<uint64_t, Reply>>(std::exchange(m_evaluations, { }));
    runPatiently("", 500, [pending] {
        for (auto& [call, reply] : *pending)
            reply->failed("eval: the page navigated away before the function returned");
    });
}

void Block::failEvaluations(std::map<uint64_t, Reply>&& evaluations, const char* message)
{
    for (auto& [call, reply] : evaluations)
        reply->failed(message);
}

// Scripts.

static bool readWorld(const Json& object, bool& isolated, std::string& error)
{
    auto world = object.get("world");
    if (!world || world.isNull()) {
        isolated = false;
        return true;
    }
    auto name = world.isString() ? world.toString() : std::string();
    if (name == "page")
        isolated = false;
    else if (name == "isolated")
        isolated = true;
    else {
        error = "unknown world " + (world.isString() ? Json::quote(name) : world.serialize()) + " (page, isolated)";
        return false;
    }
    return true;
}

void Block::setScripts(const Json& operation, const Reply& reply)
{
    auto list = operation.get("scripts");
    if (!list.isArray()) {
        reply->invalid("scripts: `scripts` must be an array");
        return;
    }
    std::vector<WebKitUserScript*> scripts;
    bool pageWorld = false;
    auto refuse = [&scripts, &reply](const std::string& message) {
        for (auto* script : scripts)
            webkit_user_script_unref(script);
        reply->invalid(message);
    };
    for (unsigned i = 0; i < list.length(); i++) {
        auto entry = list.at(i);
        auto source = entry.stringMember("source");
        if (!source) {
            refuse("`source` must be a string");
            return;
        }
        bool isolated = false;
        std::string error;
        if (!readWorld(entry, isolated, error)) {
            refuse(error);
            return;
        }
        auto frames = entry.get("frames");
        auto framesName = frames.isString() ? frames.toString() : std::string(frames && !frames.isNull() ? "?" : "main");
        if (framesName != "main" && framesName != "all") {
            refuse("scripts: unknown frames " + (frames.isString() ? Json::quote(framesName) : frames.serialize()) + " (main, all)");
            return;
        }
        auto at = entry.get("at");
        auto atName = at.isString() ? at.toString() : std::string(at && !at.isNull() ? "?" : "start");
        if (atName != "start" && atName != "end") {
            refuse("scripts: unknown at " + (at.isString() ? Json::quote(atName) : at.serialize()) + " (start, end)");
            return;
        }
        pageWorld |= !isolated;
        auto injectedFrames = framesName == "all" ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
        auto injectionTime = atName == "start" ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
        scripts.push_back(isolated
            ? webkit_user_script_new_for_world(source->c_str(), injectedFrames, injectionTime, isolatedWorldName, nullptr, nullptr)
            : webkit_user_script_new(source->c_str(), injectedFrames, injectionTime, nullptr, nullptr));
    }
    // The handlers are there before the scripts that post to them.
    setPageWorldHandlers(pageWorld);
    // The array replaces the scripts set before; the host's own script stays.
    for (auto* script : std::exchange(m_scripts, std::move(scripts))) {
        webkit_user_content_manager_remove_script(m_contentManager.get(), script);
        webkit_user_script_unref(script);
    }
    for (auto* script : m_scripts)
        webkit_user_content_manager_add_script(m_contentManager.get(), script);
    reply->ok();
}

// Evaluation.

// The async function body calling `function` (a function expression's source) with the arguments, awaiting a
// returned promise. It returns text tagged by its first letter: `u` undefined, `v` JSON, `n` not JSON (why),
// `e` thrown (what, with the stack).
static std::string evaluationBody(const std::string& function)
{
    return "const f = (\n" + function + "\n);\nlet value;\ntry { value = await f.apply(undefined, JSON.parse(" + argumentsName + ")); }\n"
        "catch (e) {\n\tif (e !== null && typeof e === 'object' && 'message' in e)\n\t\treturn 'e' + (e.name || 'Error') + ': ' + e.message + '\\n' + (e.stack || '');\n"
        "\treturn 'eUncaught ' + String(e);\n}\nif (value === undefined) return 'u';\nlet text;\n"
        "try { text = JSON.stringify(value); } catch (e) { return 'n' + String(e && e.message || e); }\n"
        "return text === undefined ? 'na ' + typeof value + ' is not JSON' : 'v' + text;";
}

void Block::run(const std::string& body, const std::string& arguments, World world, RunHandler&& then)
{
    // The dictionary is always given: without one the engine does not run the body.
    GVariantDict dictionary;
    g_variant_dict_init(&dictionary, nullptr);
    GChars valid(g_utf8_make_valid(arguments.data(), static_cast<gssize>(arguments.size())));
    g_variant_dict_insert_value(&dictionary, argumentsName, g_variant_new_string(valid.get()));
    auto* variant = g_variant_ref_sink(g_variant_dict_end(&dictionary));
    webkit_web_view_call_async_javascript_function(m_view.get(), body.data(), static_cast<gssize>(body.size()), variant,
        world == World::Isolated ? isolatedWorldName : nullptr, nullptr, nullptr, asyncReady, asyncData([then = std::move(then)](GObject* view, GAsyncResult* result) {
            GError* error = nullptr;
            auto* value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(view), result, &error);
            if (!value) {
                if (!error)
                    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "the script failed");
                then(std::nullopt, error);
                g_error_free(error);
                return;
            }
            auto text = messageText(value);
            g_object_unref(value);
            then(std::move(text), nullptr);
        }));
    g_variant_unref(variant);
}

void Block::runPatiently(const std::string& body, unsigned milliseconds, std::function<void()>&& then)
{
    // A page held by a dialog answers nothing meanwhile.
    Once once(std::move(then));
    auto timer = std::make_shared<guint>(0);
    *timer = runAfter(milliseconds, [once, timer] {
        *timer = 0;
        once();
    });
    run(body, "[]", World::Isolated, [once, timer](std::optional<std::string>&&, GError*) {
        if (auto source = std::exchange(*timer, 0))
            g_source_remove(source);
        once();
    });
}

void Block::answerEvaluation(const Reply& reply, const std::string& tagged)
{
    char tag = tagged.empty() ? '\0' : tagged[0];
    std::string rest = tagged.empty() ? std::string() : tagged.substr(1);
    switch (tag) {
    case 'u':
        reply->ok();
        return;
    case 'v': {
        // The page's own JSON.stringify wrote the text, and a page can replace that function: the text goes
        // into the answer as it is only if it is JSON, and as a string otherwise.
        if (Json::parse(rest))
            reply->ok("{\"value\":" + rest + "}");
        else
            reply->ok("{\"value\":" + Json::quote(rest) + "}");
        return;
    }
    case 'n':
        reply->fail("js", "eval: the result is not JSON: " + rest);
        return;
    case 'e':
        reply->fail("js", rest);
        return;
    default:
        reply->fail("js", "eval: unexpected result " + Json::quote(tagged));
        return;
    }
}

void Block::evaluate(const Json& operation, const Reply& reply)
{
    auto function = operation.stringMember("function");
    if (!function) {
        reply->invalid("`function` must be a string");
        return;
    }
    std::string arguments = "[]";
    if (auto member = operation.get("args"); member && !member.isNull()) {
        if (!member.isArray()) {
            reply->invalid("eval: `args` must be an array");
            return;
        }
        arguments = member.serialize();
    }
    bool isolated = false;
    std::string error;
    if (!readWorld(operation, isolated, error)) {
        reply->invalid(error);
        return;
    }
    std::vector<unsigned> path;
    if (auto frame = operation.get("frame"); frame && !frame.isNull()) {
        if (!frame.isString()) {
            reply->invalid("`frame` must be a string");
            return;
        }
        auto name = frame.toString();
        auto found = m_frames.find(name);
        if (found == m_frames.end()) {
            reply->failed("eval: no frame named " + Json::quote(name));
            return;
        }
        if (!found->second) {
            reply->failed("eval: the frame named " + Json::quote(name) + " cannot be reached");
            return;
        }
        path = *found->second;
    }

    reply->setUnansweredMessage("eval: the page went away");
    auto body = evaluationBody(*function);
    if (path.empty()) {
        uint64_t call = ++m_lastEvaluation;
        m_evaluations.emplace(call, reply);
        run(body, arguments, isolated ? World::Isolated : World::Page, [weakThis = weak_from_this(), call, reply](std::optional<std::string>&& result, GError* error) {
            if (auto block = weakThis.lock())
                block->m_evaluations.erase(call);
            if (error) {
                // A function that does not parse, or one the engine could not run.
                reply->fail(error->domain == WEBKIT_JAVASCRIPT_ERROR ? "js" : "failed", error->message);
                return;
            }
            if (!result) {
                reply->failed("eval: the page gave no result");
                return;
            }
            answerEvaluation(reply, *result);
        });
        return;
    }

    // A frame other than the main one: the call is posted to its isolated world, which runs it and posts the result.
    uint64_t call = ++m_lastFrameCall;
    std::string routed = "[[";
    for (size_t i = 0; i < path.size(); i++)
        routed += (i ? "," : "") + std::to_string(path[i]);
    routed += "],{\"id\":\"" + std::to_string(call) + "\",\"body\":" + Json::quote(body) + ",\"args\":" + Json::quote(arguments) + ",\"world\":\"" + (isolated ? "isolated" : "page") + "\"}]";
    m_frameCalls.emplace(call, reply);
    auto route = replaceAll(replaceAll(routeScript, "__TOKEN__", m_host.frameToken()), "__ARGS__", argumentsName);
    run(route, routed, World::Isolated, [weakThis = weak_from_this(), call](std::optional<std::string>&&, GError* error) {
        auto block = weakThis.lock();
        if (!block || !error)
            return;
        auto found = block->m_frameCalls.find(call);
        if (found == block->m_frameCalls.end())
            return;
        auto reply = std::move(found->second);
        block->m_frameCalls.erase(found);
        reply->failed(error->message);
    });
}

} // namespace AgentHost
