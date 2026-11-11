# Installing

An engine is one directory: everything in it finds the rest beside itself, wherever the directory is.

| File | What it is |
|---|---|
| `ABHost` (`ABHost.exe`) | the host clients start ([protocol.md](protocol.md)) |
| `extensions/` | the host's web process extension (`libABHostExtension.so`, `ABHostExtension.dll`) |
| `libWPEWebKit-2.0.1.dylib`, `libWPEWebKit-2.0.so.1`, `WPEWebKit-2.0.dll` | the engine's library |
| `ABWebProcess`, `ABNetworkProcess` (`.exe`) | its helper processes, which it looks for beside itself |
| `libWPEInjectedBundle.*` | the engine's own web process bundle |
| `ABView` (macOS) | the viewer ([viewer.md](viewer.md)) |
| `inspector.gresource` | the Web Inspector's frontend, for `--inspector` |
| `lib/` and the remaining libraries | the engine's dependencies: TLS backend, GStreamer and its plugins, fontconfig's configuration, and what they link |

## Where

The engine is installed for a user at `~/.o2/lib/abkit` (`%USERPROFILE%\.o2\lib\abkit` on Windows). Clients
look for the host there:

- **omp** runs `~/.o2/lib/abkit/ABHost` for its `webkit` browser backend. `PI_BROWSER_WEBKIT_PATH`, then the
  `browser.webkitPath` setting, name another host executable or engine directory; `PI_BROWSER_WEBKIT=0` turns the
  backend off.
- **Tern** loads the engine from `~/.o2/lib/abkit` (on macOS it runs `ABHost` there, on Linux and Windows it loads
  the library in process). `STENCIL_WEBKIT_DIR` names another engine directory; `STENCIL_WEBKIT` picks the engine.

## From a release

Releases are published to [build.stencil.so](https://build.stencil.so) as product `abkit`: one archive per target,
holding the engine directory. Unpack it as `~/.o2/lib/abkit`:

```sh
mkdir -p ~/.o2/lib && rm -rf ~/.o2/lib/abkit
curl -fL https://build.stencil.so/d/abkit/latest/macos-arm64 | tar -xz -C ~/.o2/lib
```

Targets: `macos-arm64`, `macos-x86_64`, `linux-x86_64`, `linux-arm64`, `linux-musl-x86_64`, `linux-musl-arm64`,
`windows-x86_64`, `windows-arm64`. The archive's top directory is `abkit/`.

## From a build

`WebKitBuild/install-engine.sh [ENGINE_DIR]` copies a packaged engine directory (default `WebKitBuild/engine`,
what [building.md](building.md) produces) to `~/.o2/lib/abkit`, replacing the installed one by rename so a running
host keeps its files.
