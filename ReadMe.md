# ABKit

ABKit (AgentBrowser Kit) is a browser engine for agents: a fork of [WebKit](https://webkit.org/)'s WPE port that
runs on macOS, Linux and Windows, plus `ABHost`, a headless browser that a parent process drives over its
standard streams. It is the engine behind [omp](https://github.com/can1357/oh-my-pi)'s `webkit` browser and
Tern's web views.

A page in ABKit cannot tell it is driven: it sees a visible, focused window, trusted input, a plausible screen,
and no automation flag, and no client script has to run in it. On macOS it is Safari to the page and to the
server: Safari's user agent, fonts and form controls, and Safari's TLS handshake, HTTP/2 settings and request
header order, with WebGL, WebGPU, media (played silently), Web Audio, WebRTC, WebAuthn and the rest of what Safari
exposes. What exactly a page sees: [docs/page.md](docs/page.md).

## Layout

| Path | What |
|---|---|
| `Source/` | WebKit, with the WPE port running on Darwin and Windows and the engine changes ABKit needs |
| `Source/WebKit/AgentHost/` | `ABHost` (namespace `AgentHost::`), its web process extension and the macOS viewer `ABView` (`View/`) |
| `WebKitBuild/` | build, packaging and install scripts per platform (`darwin/`, `linux/`, `win/`), the CI smoke test |
| `docs/` | the host's protocol and how to build and install the engine |
| `.github/workflows/release.yml` | builds every target and publishes releases |

The library keeps WPE's name (`libWPEWebKit-2.0`); the processes are ABKit's: `ABHost`, `ABWebProcess`,
`ABNetworkProcess`, `ABView`.

## Using it

A client starts `ABHost` from the engine directory and speaks JSON frames to it:

```text
→ {"hello":{}}
← {"welcome":{"ops":["browser","dom","console","network","present"]}}
→ {"id":1,"browser":{"op":"open","url":"https://example.com"}}
← {"id":1,"browser":{"ok":{"block":1,"url":"https://example.com"}}}
→ {"id":2,"browser":{"op":"events","block":1,"wait":true}}
← {"id":2,"browser":{"ok":{"events":[…,{"type":"loaded","url":"https://example.com/","seq":7}],"next":7,"dropped":0}}}
→ {"id":3,"browser":{"op":"eval","block":1,"function":"() => document.title"}}
← {"id":3,"browser":{"ok":{"value":"Example Domain"}}}
```

Each frame is a 4-byte little-endian length and that much UTF-8 JSON. `WebKitBuild/smoke-engine.py` is a complete
client in a hundred lines.

- **omp** runs the installed engine for its browser tool whenever it is installed (`browser.open` without `app`,
  or `app: { webkit: true }`), and shows a tab in a window through `ABView` on macOS.
- **Tern** draws its web views with it: on macOS as blocks of an `ABHost` child process presented over Mach
  (`STENCIL_WEBKIT=agent`, the default once installed), elsewhere by loading the library in process.

## Documentation

- [docs/protocol.md](docs/protocol.md): the host's process, wire format and ops
- [docs/events.md](docs/events.md): what a block reports
- [docs/page.md](docs/page.md): what a page sees
- [docs/presentation.md](docs/presentation.md): showing a block's frames in another process, and the Mach channel
- [docs/viewer.md](docs/viewer.md): `ABView`, the macOS window client
- [docs/building.md](docs/building.md): building and packaging the engine for each platform
- [docs/installing.md](docs/installing.md): the engine directory, where clients look for it, releases

## Building and installing

On a Mac with Homebrew:

```sh
WebKitBuild/darwin/build-engine.sh darwin-arm64 WebKitBuild/engine
WebKitBuild/install-engine.sh
```

The engine lands in `~/.o2/lib/abkit`, where omp and Tern find it. Linux and Windows: [docs/building.md](docs/building.md).
Releases for every platform are on [build.stencil.so](https://build.stencil.so) (product `abkit`).

## Upstream

ABKit tracks [WebKit/WebKit](https://github.com/WebKit/WebKit) `main`. WebKit's own documentation is in
[Introduction.md](Introduction.md); its licenses (LGPL 2 and BSD, per file) apply to ABKit unchanged.
