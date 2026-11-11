# ABHost protocol

`ABHost` runs headless web views for a parent process, which drives them over the host's standard streams.
This file, with [events.md](events.md), [page.md](page.md), [presentation.md](presentation.md) and
[viewer.md](viewer.md), is the contract between the host and its clients (omp's `webkit` browser backend and
Tern's `agent` engine are two).

## Process

- `ABHost [--data-dir DIR] [--inspector HOST:PORT]`. `DIR` keeps website data per profile across runs;
  without it every profile lives in a temporary directory that is removed when the host exits.
- `--inspector HOST:PORT` serves the Web Inspector for the host's views: open `http://HOST:PORT` in a browser and
  pick a view from the target list, and the views' pages enable the instrumentation that makes them debuggable.
  The views serve from the engine's own resources, beside its library. It is off by default (no inspector server
  runs and the views are not inspectable) and binds only the given address, a port alone meaning `127.0.0.1`.
- Requests arrive on standard input and answers leave on standard output, as frames of a 4-byte little-endian
  payload length followed by UTF-8 JSON (at most 256 MiB). Nothing else is written to standard output: the host
  keeps a private duplicate for the protocol and points descriptor 1 at standard error. Diagnostics go to
  standard error (`G_MESSAGES_DEBUG=ABHost` adds the host's debug log).
- The host exits with status 0 when standard input ends, after closing every block. It reads no environment
  variables and finds the engine through its rpath, and its web process extension in `extensions/` beside its
  own executable.

## Envelope

- `{"hello":{}}` is answered `{"welcome":{"ops":["browser","dom","console","network","present"]}}`. `dom` says
  the `dom` op exists; `console` and `network` say those event families are reported; `present` (macOS only)
  says blocks can be presented to another process ([presentation.md](presentation.md)).
- `{"id":N,"browser":{"op":NAME,…}}` is answered `{"id":N,"browser":{"ok":VALUE}}` or
  `{"id":N,"browser":{"error":{"kind":K,"message":M}}}`, in any order. Kinds: `invalid` (bad or missing field,
  unknown op), `not_found` (no such block), `unsupported`, `js` (an `eval` threw), `failed` (anything else).
- `{"id":N,"fork":…}` is answered with an `unsupported` error: the host has no sessions.

## Blocks

A block is one web view. Ids are positive integers, never reused in a process. Every op but `open`, `blocks`,
`clipboard` and `clear` takes `block`.

| Op | Fields | Answer |
|---|---|---|
| `open` | `url` (default `about:blank`), `width`, `height` (CSS px, 1..16384, default 1280×800), `scale` (device px per CSS px, 0.25..8, default 1), `profile` (name, default `"agent"`; `null` for a store of its own); `owner` and `show` are accepted and ignored | `{"block","url"}` once the view exists |
| `close`, `release` | | `{}`; the view is destroyed |
| `blocks` | | `{"blocks":[{"block","url","title","owner":null,"shown","driven":true}]}`; `shown` is true while the block is presented |
| `state` | | `{"url","title","loading","progress","back","forward","width","height","shown","focused","error"}`; `shown` is true while the block is presented, `focused` is what `focus` last said (default true) |
| `goto` | `url` | `{}` at once; the load is reported by events |
| `nav` | `go`: `back`, `forward`, `reload`, `stop` | `{}` at once |
| `viewport` | `width`, `height`, `scale` (as for `open`; unchanged when left out) | `{}` |
| `events` | `after` (default 0), `wait` (default false) | `{"events":[…],"next","dropped"}`; each event has `seq`, counted from 1; the last 4096 are kept. With `wait`, answered only once there is an event after `after` (or with `not_found` when the block closes); one waiting call per block: a new `events` call answers the waiting one with what it has (possibly nothing). The events: [events.md](events.md) |
| `focus` | `value` (boolean) | `{}`; whether the view holds the keyboard. True (the default) is what a driven page must see: focused in an active window whatever happens. A presenting client sets false while its block does not have the keyboard |
| `present` | `to`: a target, or `null` to stop | `{}` once the target is reached; `failed` when it cannot be ([presentation.md](presentation.md)) |
| `clear` | `profile` (a name) | `{}` once that profile's website data (cookies, storage, caches) is erased; a profile never opened, or without data, is no error; `null` is `invalid` (a store of a block's own goes with its block). Blocks open on the profile stay open and find the data gone |

`open` with `about:blank` loads nothing, so the first `goto` is the view's first history entry; `committed` and
`loaded` are still reported for it.

## Page

| Op | Fields | Answer |
|---|---|---|
| `dom` | a request of `webkit_web_view_run_interaction_query()` (`read`: `snapshot`, `resolve`, `query`, `wait`) | the engine's reply, verbatim, also when it is an `{"error":…}` reply |
| `eval` | `function` (source), `args` (array), `world` (`page` or `isolated`), `frame` (a frame's name) | `{}` for `undefined`, else `{"value"}`; a throw, or a result that is not JSON, is kind `js` |
| `scripts` | `scripts`: `[{"source","world","frames":"main"\|"all","at":"start"\|"end"}]`, replacing the list | `{}` |
| `input` | `events`: steps `mouse` (`action` `move`\|`down`\|`up`, `x`, `y`, `button`, `clicks`, `mods`), `wheel` (`x`, `y`, `dx`, `dy`, `mods`), `key` (`action`, `key`, `code`, `mods`), `text` (`text`) | `{}` two frames after the last step |
| `edit` | `action`: `copy`, `cut`, `paste`, `selectAll`, `undo`, `redo` | `{}` |
| `capture` | `format` (`png`, `jpeg`), `quality`, `scale`, `rect` (`{x,y,width,height}`), `full` | `{"data","mime","width","height"}` |
| `dialogs` | `policy`: `default` (alerts and beforeunload accepted, confirm and prompt held), `accept`, `dismiss` | `{}` |
| `dialog` | `accept`, `text` | `{}`; `failed` when no dialog is held |
| `files` | `paths` (array, or `null`) | `{}`; answers a held chooser or presets the next one |
| `cookies` | | `{"cookies":[{"name","value","domain","path","expires","httpOnly","secure","sameSite"}]}` |
| `setCookie`, `deleteCookie` | `cookie`; `name`, `domain`, `path` | `{}` |
| `agent` | `value` (string, or `null` for the host's default) | `{}` |
| `appearance` | `value`: `dark`, `light`, `null` | `{}`; applies to every block of the process. `null` (the default) is the system's appearance: on macOS the Mac's own, dark or light, as in Safari; light elsewhere |
| `allow` | `hosts` (exact names and `*.domain`, or `null`) | `{}` |
| `insecure` | `value` | `{}`; `true` accepts invalid certificates |
| `fileAccess` | `value` | `{}`; `true` lets a `file:` document read other files (fetch, XHR, frames, canvas pixels) as if they shared its origin, as Chromium's `--allow-file-access-from-files` does. Off by default; documents loaded afterwards see the change |
| `credentials` | `username`, `password`, or neither | `{}` |
| `downloads` | `dir` (or `null`: downloads are cancelled) | `{}` |
| `identity` | `seed`, `screen`, `window` ([page.md](page.md)) | `{}` |
| `clipboard` | `text` to write, nothing to read | `{}` or `{"text"}`; the host's own clipboard, not the system's |

Input steps are trusted events sent through the view (`text` is an input-method commit, without key events).
Key names are DOM `key` values; a key the host cannot map fails the call with `input: unknown key "…"`.
`pdf` and `media` answer `unsupported`.
