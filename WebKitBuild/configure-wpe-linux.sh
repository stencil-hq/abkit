#!/bin/sh
# Configures the WPE port for Linux with the macOS feature set (configure-wpe-darwin.sh) wherever
# Linux can have it. Run inside the build container (linux/build-engine.sh), after
# linux/build-deps.sh has filled DEPS_PREFIX.
# Usage: configure-wpe-linux.sh BUILD_DIR DEPS_PREFIX [extra cmake args]
#
# On, as on macOS: media, Web Audio, MediaStream, WebRTC (the bundled libwebrtc), MSE, WebCodecs,
# Encrypted Media and the legacy WebKitMediaKeys API, through GStreamer without its GL sink;
# WebGL (ANGLE over EGL; the package brings Mesa's llvmpipe as that EGL); gamepads
# (libmanette), pointer lock, the PDF.js viewer, the Web Inspector server, DeviceMotion,
# DeviceOrientation and GestureEvent, Picture in Picture and the video presentation and wireless
# playback interfaces, <model>'s interface and navigator.standalone; speech synthesis, which speaks
# through Flite here, linked in with its voices (macOS has the system's synthesizer).
# On where macOS has it off for want of the library: color management (lcms2).
#
# Off, as on macOS: the GPU process and DMA-BUF frames (GBM; frames cross as shared memory; libdrm
# is on only for the DRM formats the Linux code names), the bubblewrap sandbox (bwrap and xdg-dbus-proxy are not relocatable), spell checking,
# WebXR, WebDriver, autocapitalize, libhyphen (its dictionaries are found by absolute path),
# ATK (the accessibility bus), libbacktrace, sysprof and journald, and Thunder.
# Off on Linux only: WebGPU, whose one implementation in the tree is Metal, and WebAuthn (with the
# Digital Credentials API it carries), whose UI process side is written against the Security
# framework.
# JPEG XL is on where the distribution has libjxl (Alpine) and off where it does not
# (Ubuntu 22.04), see USE_JPEGXL below.
#
# libsoup, GLib (glibc) and Mesa come from DEPS_PREFIX, ahead of the distribution's.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
build=$1
deps=$2
[ -n "$build" ] && [ -f "$deps/lib/pkgconfig/libsoup-3.0.pc" ] || { echo "usage: $0 BUILD_DIR DEPS_PREFIX (run linux/build-deps.sh first)" >&2; exit 2; }
shift 2
PKG_CONFIG_PATH=$deps/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
export PKG_CONFIG_PATH
if pkg-config --exists libjxl; then jpegxl=ON; else jpegxl=OFF; fi
# musl has gettext in its C library, but with GNU gettext's <libintl.h> installed (GLib's headers
# bring it) the calls name libintl's functions, which nothing else links.
# Alpine's clang also turns on _FORTIFY_SOURCE, whose fortify-headers trap on a memcpy() Skia's
# glyph cache makes past the declared size of its descriptor storage, so it is turned off as the
# glibc build has it.
# With glibc, relative relocations stay unpacked: DT_RELR needs glibc 2.36
# (GLIBC_ABI_DT_RELR), and the engine is to run on the 2.35 it is built against.
ldflags=-fuse-ld=lld
cflags=
relr=OFF
case $(ldd --version 2>&1) in
*musl*)
	ldflags="$ldflags -lintl"
	cflags=-U_FORTIFY_SOURCE
	relr=ON
	;;
esac
# Under ccache the precompiled headers are off, as for the Windows build and WebKit's own
# developer builds: a cached header outlives a change of cmakeconfig.h, which clang then refuses,
# and objects hit the cache without them anyway.
launcher=
if command -v ccache >/dev/null 2>&1; then
	launcher="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON"
fi
# fontconfig's location is looked up afresh, so a build directory configured before the prefix
# had its own (glibc) moves over to it.
exec cmake -S "$root" -B "$build" -G Ninja $launcher \
	-U 'Fontconfig_*' -U 'PKG_FONTCONFIG*' -U 'pkgcfg_lib_PKG_FONTCONFIG*' \
	-DPORT=WPE -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$deps" \
	-DCMAKE_C_FLAGS="$cflags" -DCMAKE_CXX_FLAGS="$cflags" -DLD_PACK_RELATIVE_RELOCS=$relr \
	-DCMAKE_EXE_LINKER_FLAGS="$ldflags" -DCMAKE_SHARED_LINKER_FLAGS="$ldflags" -DCMAKE_MODULE_LINKER_FLAGS="$ldflags" \
	-DENABLE_TOOLS=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_MINIBROWSER=OFF \
	-DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
	-DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
	-DUSE_ATK=OFF -DUSE_FLITE=ON -DUSE_GBM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBDRM=ON \
	-DUSE_LIBHYPHEN=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_LCMS=ON -DUSE_JPEGXL=$jpegxl \
	-DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_VIDEO=ON -DENABLE_WEB_AUDIO=ON -DUSE_GSTREAMER=ON -DUSE_GSTREAMER_GL=OFF \
	-DENABLE_MEDIA_STREAM=ON -DENABLE_MEDIA_RECORDER=ON -DENABLE_WEB_CODECS=ON -DENABLE_MEDIA_SOURCE=ON -DENABLE_MEDIA_SOURCE_IN_WORKERS=ON \
	-DENABLE_WEB_RTC=ON -DENABLE_WEBGPU=OFF \
	-DENABLE_SPEECH_SYNTHESIS=ON -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=ON -DENABLE_WEB_AUTHN=OFF \
	-DENABLE_WEBDRIVER=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_PDFJS=ON -DENABLE_THUNDER=OFF \
	-DENABLE_WEBXR=OFF -DENABLE_ENCRYPTED_MEDIA=ON -DENABLE_LEGACY_ENCRYPTED_MEDIA=ON -DENABLE_WEBGL=ON \
	-DENABLE_POINTER_LOCK=ON -DENABLE_REMOTE_INSPECTOR=ON \
	-DENABLE_PICTURE_IN_PICTURE_API=ON -DENABLE_VIDEO_PRESENTATION_MODE=ON -DENABLE_WIRELESS_PLAYBACK_TARGET=ON \
	-DENABLE_DEVICE_ORIENTATION=ON -DENABLE_MAC_GESTURE_EVENTS=ON \
	-DENABLE_AUTOCAPITALIZE=OFF \
	-DENABLE_MODEL_ELEMENT=ON \
	-DENABLE_NAVIGATOR_STANDALONE=ON \
	"$@"
