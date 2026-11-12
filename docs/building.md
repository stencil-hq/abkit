# Building

Every platform builds the WPE port with CMake and Ninja and packages the result as one relocatable engine
directory ([installing.md](installing.md)). One script per platform does all of it, as CI runs it:

```sh
WebKitBuild/<darwin|linux|win>/build-engine.sh TARGET OUT_DIR
```

`TARGET` is one of `darwin-arm64`, `darwin-x64`, `linux-x64`, `linux-arm64`, `linux-musl-x64`, `linux-musl-arm64`,
`windows-x64`, `windows-arm64`. With `CCACHE_DIR` set, compiles go through ccache. `WebKitBuild/smoke-engine.py
OUT_DIR` then checks the package: the host starts, loads a page, runs script, captures a frame and exits cleanly.

CI (`.github/workflows/release.yml`) runs the eight builds on GitHub's hosted runners, smoke-tests each on its own
system, and on a `v<version>` tag publishes the archives to build.stencil.so.

## macOS

`WebKitBuild/darwin/build-engine.sh darwin-arm64|darwin-x64 OUT_DIR`, on a Mac with Xcode (or its command line
tools) and Homebrew. Homebrew no longer builds for Intel Macs, so on Apple silicon darwin-x64 takes the Brewfile's
last x86_64 bottles from ghcr.io, poured into `/usr/local` (`WebKitBuild/darwin/x86-homebrew.py`, with `sudo`), and
runs the steps below under Rosetta, with Apple silicon's clang compiling for x86_64. Its steps can be run alone:

1. `brew bundle --file=WebKitBuild/darwin/Brewfile`: the libraries and tools.
2. `WebKitBuild/darwin/build-deps.sh`: libsoup with Safari's HTTP/2 preface, ALPN list and request header order
   (`patches/libsoup-safari.patch`), and glib-networking's TLS backend on BoringSSL with Safari's ClientHello
   (`Source/GIOBoringSSL`, `patches/`), into `WebKitBuild/darwin/prefix`. Sources are pinned by version and
   checksum.
3. `WebKitBuild/configure-wpe-darwin.sh [cmake args]` configures `WebKitBuild/wpe-darwin`.
4. `ninja -C WebKitBuild/wpe-darwin`.
5. `WebKitBuild/package-engine.sh WebKitBuild/wpe-darwin OUT_DIR`: the library, helpers, host, viewer and
   extension, libsoup and the TLS backend, the inspector's frontend, and every Homebrew library they load, rewired
   to `@rpath` inside the package; GStreamer's plugins and scanner, and fontconfig's configuration, which the
   engine finds through `useBundledRuntimePathsIfPresent()` (`Source/WebCore/platform/graphics/BundledRuntime.cpp`).
   Plugins for desktop toolkits, X11, GL and Python are left out.

The engine has what Safari has: media through GStreamer (played silently), Web Audio, MediaStream, WebRTC (the
bundled libwebrtc), MSE, WebCodecs, Encrypted Media (ClearKey), WebGL and WebGPU, WebAuthn, gamepads, pointer
lock, PDF.js, speech, device orientation and gesture events, `<model>`; `configure-wpe-darwin.sh` says why each is
on or off. It bundles Homebrew's libraries, so it runs on the macOS version it was built on and later. It draws
with Metal (through ANGLE), so it needs a Metal device: every Mac that runs a current macOS has one, but a virtual
machine without a GPU (GitHub's Intel macOS runners) does not, and there pages fail to load. CI builds and tests
both targets on Apple-silicon Macs.

## Linux

`WebKitBuild/linux/build-engine.sh linux-x64|linux-arm64|linux-musl-x64|linux-musl-arm64 OUT_DIR` builds in a
container; the host needs only Docker (CI: vmon for x64, Blacksmith for arm64; another architecture works through
emulation, slowly). The container is `WebKitBuild/linux/Dockerfile.glibc` (Ubuntu 22.04, clang 20) or
`Dockerfile.musl` (Alpine 3.22, with apt.llvm.org's clang 20 on its own glibc compiling for Alpine: under musl's
allocator clang builds the engine about 1.8 times slower), and in it:

1. `WebKitBuild/linux/build-deps.sh` builds pinned sources: the same Safari-shaped libsoup and BoringSSL TLS
   module as macOS (shared steps in `WebKitBuild/safari-deps.sh`, plus `linux/patches/`), Mesa for EGL/GLES on
   llvmpipe, and on glibc a newer GLib, Brotli and fontconfig.
2. `WebKitBuild/configure-wpe-linux.sh BUILD_DIR DEPS_PREFIX`, then `ninja`.
3. `WebKitBuild/package-engine-linux.sh BUILD_DIR OUT_DIR DEPS_PREFIX`: every library but the C library (and on
   glibc libstdc++ and libgcc_s) goes into `lib/` behind `$ORIGIN` run paths, with GStreamer's plugins and
   scanner, fontconfig's configuration and DejaVu as a last-resort font, xkb keymaps and (musl) ICU data, found
   through `useBundledRuntimePathsIfPresent()`. Packaging fails if `ldd` reports a missing library or symbol
   version.

A glibc engine runs on glibc 2.35 and later (Ubuntu 22.04, Debian 12, Fedora 36 and newer) with the system's
libstdc++; a musl engine on musl 1.2.5 and later with nothing else from the system. The feature set is macOS's
(media through GStreamer, Web Audio, MediaStream, WebRTC, MSE, WebCodecs, ClearKey Encrypted Media through
OpenSSL, WebGL, PDF.js, speech through Flite, and Safari's TLS handshake), drawn in software. Off on Linux:
WebGPU (the tree's only implementation is Metal's), WebAuthn (its UI-process code is Cocoa's), the bubblewrap
sandbox and libhyphen (neither relocates). The Safari-matching runtime defaults of macOS apply on Linux too.

## Windows

`WebKitBuild/win/build-engine.sh windows-x64|windows-arm64 OUT_DIR` cross-compiles from Linux (CI: ubuntu-24.04
for both targets) with clang-cl and lld-link; on Ubuntu it installs what is missing itself (apt, LLVM from
apt.llvm.org, `xwin`), elsewhere the header of the script lists what it needs (macOS: Homebrew's llvm and lld,
and `xwin`). Its steps:

1. `xwin` splats the Windows SDK and the MSVC CRT (accepting Microsoft's license); `WebKitBuild/win/fetch-deps.py
   x64|arm64` fetches MSYS2's `clang64`/`clangarm64` GLib stack (GLib, libsoup, HarfBuzz, ICU, fontconfig, …)
   and clang's Windows builtins. All of them are C libraries on the UCRT, so the MSVC-ABI build links them as
   they are.
2. `WebKitBuild/configure-wpe-win.sh x64|arm64` configures `WebKitBuild/wpe-win-ARCH` with
   `WebKitBuild/win/toolchain.cmake`.
3. `ninja -C WebKitBuild/wpe-win-ARCH`.
4. `WebKitBuild/package-engine-win.sh x64|arm64 BUILD_DIR OUT_DIR`: the DLL, helpers, `ABHost.exe`,
   `extensions/ABHostExtension.dll`, the inspector's frontend, every DLL they import from the build or the MSYS2
   sysroot, GIO's OpenSSL TLS module (over the Windows certificate store) and fontconfig's configuration, which
   GIO and fontconfig find relative to their own DLLs.

The CRT is linked statically ("hybrid CRT": vcruntime and the C++ library in every module, the UCRT from
Windows), so the engine needs no Visual C++ redistributable and every module shares one heap. With `CCACHE_DIR`
set, compiles go through ccache, provided the source path does not begin like a clang-cl option (`/Users`,
`/work`).

Off on Windows, each needing port work: media (GStreamer, and with it Web Audio, MediaStream, WebRTC, MSE,
WebCodecs, Encrypted Media), WebGL and WebGPU, the GPU process, gamepads, WebAuthn and speech. TLS is MSYS2's
stock libsoup and OpenSSL, not Safari's handshake. The web process draws with Direct3D 11, falling back to the WARP
software renderer where there is no hardware device.
