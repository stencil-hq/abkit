# WPEAgentHost protocol

`WPEAgentHost` runs headless web views for a parent process, which drives them over the host's standard
streams. This file is the contract between the host and its clients (omp's `webkit` browser backend is one).

## Process

- `WPEAgentHost [--data-dir DIR] [--inspector HOST:PORT]`. `DIR` keeps website data per profile across runs;
  without it every profile lives in a temporary directory that is removed when the host exits.
- `--inspector HOST:PORT` serves the Web Inspector for the host's views: open `http://HOST:PORT` in a browser and
  pick a view from the target list, and the views' pages enable the instrumentation that makes them debuggable.
  The views serve from the engine's own resources, beside its library. It is off by default (no inspector server
  runs and the views are not inspectable) and binds only the given address, a port alone meaning `127.0.0.1`.
- Requests arrive on standard input and answers leave on standard output, as frames of a 4-byte little-endian
  payload length followed by UTF-8 JSON (at most 256 MiB). Nothing else is written to standard output: the host
  keeps a private duplicate for the protocol and points descriptor 1 at standard error. Diagnostics go to
  standard error.
- The host exits with status 0 when standard input ends, after closing every block. It reads no environment
  variables and finds the engine through its rpath, and its web process extension in `extensions/` beside its
  own executable.

## Envelope

- `{"hello":{}}` is answered `{"welcome":{"ops":["browser","dom","console","network","present"]}}`. `dom` says
  the `dom` op exists; `console` and `network` say those event families are reported; `present` (macOS only)
  says blocks can be presented to another process (below).
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
| `events` | `after` (default 0), `wait` (default false) | `{"events":[…],"next","dropped"}`; each event has `seq`, counted from 1; the last 4096 are kept. With `wait`, answered only once there is an event after `after` (or with `not_found` when the block closes); one waiting call per block: a new `events` call answers the waiting one with what it has (possibly nothing) |
| `focus` | `value` (boolean) | `{}`; whether the view holds the keyboard. True (the default) is what a driven page must see: focused in an active window whatever happens. A presenting client sets false while its block does not have the keyboard |
| `present` | `to`: a target, or `null` to stop | `{}` once the target is reached; `failed` when it cannot be (below) |
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
| `credentials` | `username`, `password`, or neither | `{}` |
| `downloads` | `dir` (or `null`: downloads are cancelled) | `{}` |
| `identity` | `seed`, `screen`, `window` (below) | `{}` |
| `clipboard` | `text` to write, nothing to read | `{}` or `{"text"}`; the host's own clipboard, not the system's |

Input steps are trusted events sent through the view (`text` is an input-method commit, without key events).
Key names are DOM `key` values; a key the host cannot map fails the call with `input: unknown key "…"`.
`pdf` and `media` answer `unsupported`.

## Events

`url`, `title`, `progress` (`value`), `history` (`back`, `forward`), `committed` (`url`), `loaded` (`url`),
`failed` (`message`), `newWindow` (`url`; nothing opens), `blocked` (`url`, a navigation `allow` refused),
`dialog` (`kind`, `message`, `default`, `url`, `handled`: `accepted`, `dismissed` or `null` while held),
`chooser` (`multiple`, `directories`, `handled`), `download` (`id`, `url`, `path`, `state`: `started`,
`finished`, `failed`, `error`), `message` (`body`, `world`, `main`, `url`; a script posted to
`webkit.messageHandlers.stencil`), `frame` (`name`, `main`, `url`; posted to `stencilFrame`) and `cursor`
(`name`, a CSS cursor keyword such as `default`, `pointer`, `text`: the page's cursor changed, reported only while
the block is presented, and once when a presentation starts).

Observed without any script in the page, each with `ts` (epoch milliseconds):

- `console`: `level` (`log`, `info`, `warning`, `error`, `debug`), `text`, `source` (`javascript` for an
  uncaught exception, `console-api`, `network`, `security`, `other`), `url`, `line`.
- `request`: `id` (unique in the block), `url`, `method`, `headers`, `main`. A redirect repeats it with the
  same `id`.
- `response`: `id`, `url`, `status`, `mime`, `headers`, `main`.
- `requestFinished`: `id`, `length` (the declared `Content-Length`, 0 when there is none).
- `requestFailed`: `id`, `url`, `error`.

## What a page sees

The host exists so that a page cannot tell it is driven. A client keeps that property by installing no
page-world script; everything below holds without one.

- The view is visible, focused and active, and its input is trusted. `navigator.webdriver` is false.
- Nothing is composited or painted to a buffer except during `capture` and while the block is presented, while
  animation frames keep their display cadence (below).
- `webkit.messageHandlers.stencil` and `stencilFrame` exist in the isolated world always, and in the page's
  world only while the `scripts` list holds a page-world script.
- The screen and the window are never the viewport. The window frame is `x: 80, y: 60`, as wide as the
  viewport and 87 px taller; the screen is the first of 1920×1080, 2560×1440, 3440×1440 and 3840×2160 that
  holds it, less a 25 px menu bar at the top on macOS and a 40 px bar at the bottom elsewhere. Both follow the
  viewport.
- The machine is a desktop: there is a mouse and a keyboard and no touchscreen, so `(hover: hover)` and
  `(pointer: fine)` match and there are no touch events or `ontouch*` attributes.
- On macOS the user agent is Safari's and `window.safari` exists. The generic font families are the ones Safari
  uses (Times, Helvetica, Courier, Apple Chancery, Papyrus; Times for a family that is not installed), and
  `system-ui` and `-apple-system` are the system font. Form controls use the system font at 11 px.
  Scrollbars overlay the page and take no room in it, as on a Mac with only a trackpad.
- On macOS the TLS handshake, the HTTP/2 settings and the request headers, their order included, are the ones
  Safari sends.
- Video and audio play and Web Audio runs, but no sound reaches an output device. There is no camera or
  microphone: `enumerateDevices()` lists one unlabelled device of each kind, as Safari does before permission,
  and `getUserMedia()` and `getDisplayMedia()` are denied. WebAuthn requests and `navigator.share()` end as a
  sheet the user dismissed, and no gamepad is attached.
- `identity` changes this: `seed` (a string; `null` for none) seeds the noise the engine adds to canvas
  read-backs, so one seed is one stable device; `screen` (`{"width","height","availLeft","availTop","availWidth",
  "availHeight"}`) and `window` (`{"x","y","width","height"}`) pin those values, and `null` returns either to
  the rule above. A member that is left out is unchanged.

## Presentation

`{"op":"present","block":N,"to":TARGET}` shows a block's frames in another process without copying them:
while presented, the block renders like a page on screen and every frame it renders goes to the target as the
buffer the web process drew it into. `{"op":"present","block":N,"to":null}` stops. Stopping, `close`, the
host's exit and the target's death end a presentation; then the buffers go back to the view and nothing is
rendered again until the next `capture` or `present`. `capture` works while presenting. Presenting a block to
the target it is presented to already does nothing; to another one, ends the first presentation.

`TARGET`:

- macOS: `{"mach":NAME}`, a Mach bootstrap service the client checked in (`bootstrap_check_in()`), which must
  outlive the presentations to it. One service serves every block, and every host, of a client. The answer
  `{}` comes once the service was reached and sent `hello`; `failed` when it cannot be reached.
- `{"hwnd":N}` (Windows) and `{"socket":PATH}` (Linux) are reserved: answered `unsupported`.

### The viewer (macOS)

The host has no window. `WPEAgentView`, beside the host in the engine's directory, is a presentation client
that gives presented blocks one each: a program a client starts, takes its service name from, and presents
blocks to. It speaks one JSON object a line on its standard streams and exits when its input closes.

- It writes `{"ready":{"mach":NAME}}` once its service is checked in: the `present` target is `{"mach":NAME}`.
  One viewer serves the blocks of one host.
- `{"show":{"block":N,"title":"…","scale":S}}` names a block's window before the block is presented: its title
  and the block's `scale`, from which the window takes its size (one point per CSS pixel, smaller where the
  screen is). The window opens with the block's first frame, in front but without taking the keyboard from the
  app the user is in, and closes when the presentation ends. `show` for a shown block renames its window.
- `{"hide":{"block":N}}` closes a block's window; the client also stops presenting the block.
- `{"input":{"block":N,"events":[…]}}` is what the user did in a window, as steps of the `input` op in the
  block's CSS pixels (`mouse`, `wheel`, `key`, and `text` for what a key press cannot say); the client sends
  them on. The viewer never sets `focus`: a shown page stays focused like a hidden one.
- `{"closed":{"block":N}}`: the user closed the window; the client stops presenting the block.
  `{"ended":{"block":N}}`: the block's presentation ended (`bye`) and its window is gone.

### Pacing

The host's pages run their animation frames at the refreshes of one screen. While a client sent a `tick` (its
display refreshed) within the last second, a refresh is a `tick`; otherwise a 60 Hz timer is. A client sends
`tick`s from its display link while it shows a presented block, and stops when it shows none. A presented frame
counts as shown at the refresh after it was sent (its page's next frame waits for that), so a presented page
renders at most one frame per refresh: with 120 ticks a second, 120 frames and animation frames a second.

### The Mach channel (macOS)

Plain `mach_msg()` messages between the host and the client's service, in native byte order. `msgh_id` is the
kind. A simple message is the 24-byte `mach_msg_header_t` followed by its payload of 32-bit unsigned words; a
complex one (`MACH_MSGH_BITS_COMPLEX`) is the header, a `mach_msg_body_t` (4 bytes, `msgh_descriptor_count`
1), one `mach_msg_port_descriptor_t` (12 bytes) and then its payload words. `msgh_size` is exactly that size:

| `msgh_id` | Name | Direction | Port descriptor | Payload (32-bit words) | `msgh_size` |
|---|---|---|---|---|---|
| 1 | `hello` | host → client | a send right (`MACH_MSG_TYPE_MAKE_SEND`) to the host's port for this client | `version` (1), `pid` of the host | 48 |
| 2 | `buffer` | host → client | the buffer's IOSurface (`IOSurfaceCreateMachPort()`, moved) | `block`, `buffer`, `width`, `height` (device px) | 56 |
| 3 | `frame` | host → client | | `block`, `buffer` | 32 |
| 4 | `gone` | host → client | | `block`, `buffer` | 32 |
| 5 | `bye` | host → client | | `block` | 28 |
| 16 | `released` | client → host | | `block`, `buffer` | 32 |
| 17 | `tick` | client → host | | none | 24 |

Byte offsets: in a simple message the words start at 24 (`block` at 24, `buffer` at 28); in a complex one the
port name is at 28 (the descriptor's `name`) and the words start at 40 (`hello`: `version` at 40, `pid` at 44;
`buffer`: `block` at 40, `buffer` at 44, `width` at 48, `height` at 52). The client's messages go to the port
`hello` gave, with `MACH_MSG_TYPE_COPY_SEND` and no reply port.

- `hello` is the first message a host sends to a service, once per host process. Its port takes the client's
  `released` and `tick`s for every block of that host.
- `buffer` comes before the first `frame` in that buffer. The client looks the surface up
  (`IOSurfaceLookupFromMachPort()`) and deallocates the right. Buffer ids are per block and never reused; a
  block makes new buffers when its size or scale changes, or when its web process is replaced.
- A surface is 32-bit BGRA, premultiplied alpha, sRGB, top row first, `width` × `height` device pixels (the
  block's CSS size times its `scale`). After `frame` the client shows it (e.g. as a `CALayer`'s contents), and
  sends `released` for it once it no longer does (typically when the next `frame` replaces it): the host draws
  into a buffer again only after that. A client reading pixels does so under `IOSurfaceLock()`, which waits for
  the GPU to finish drawing.
- `gone`: the host destroyed that buffer; the client lets go of its surface (not sending `released` for it).
- `bye`: the presentation of that block ended; all its buffers are gone, released or not.
- The host sends with a one-second timeout: a client keeps its service's queue large enough
  (`MACH_PORT_QLIMIT_LARGE`) and drains it. A client that dies, or whose queue stays full, is gone: its
  presentations end as with `"to":null`, without `bye`.
