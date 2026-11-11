#!/bin/sh
# Configures the WPE port for a macOS host with every Linux-only piece off.
# Usage: configure-wpe-darwin.sh [extra cmake args]
#
# On, as in Safari: media, Web Audio, MediaStream, WebRTC, MSE, WebCodecs
# (GStreamer from Homebrew, without its GL sink, and the bundled libwebrtc),
# WebGL and WebGPU (ANGLE and the Metal backend, in the web process),
# WebAuthn, gamepads, pointer lock, the PDF.js viewer, and the DeviceMotion,
# DeviceOrientation and GestureEvent interfaces (a Mac has no motion or
# orientation sensors and the headless host sends no trackpad gestures, so the
# events exist without ever firing, as in Safari on a Mac).
# On as well: the <model> element's interface. No model player is linked into
# this port, so a <model> with a source ends the way it does with no player at
# all: its ready promise rejects and its fallback content renders.
# Speech is on as well: synthesis speaks through the system's synthesizer and
# the recognition interface is present, ending in the permission error Safari
# ends in where the microphone has not been granted.
#
# Off: the GPU process and DMA-BUF frames (frames cross as IOSurfaces or
# shared memory), spell checking, WebXR, and autocapitalize, which macOS
# system WebKit does not have either.
#
# Encrypted Media is on, answered by the in-tree ClearKey CDM as Safari answers
# for org.w3.clearkey, and decrypted by this port's own webkitclearkey element
# (AES-128-CTR, CommonCrypto); Thunder is off, so no other key system is
# advertised.
# The legacy WebKitMediaKeys API is built too, so its interfaces exist as they
# do in Safari, but it claims no key system: the media engine has no path from a
# legacy CDM session to a decryptor.
#
# libsoup comes from WebKitBuild/darwin/prefix (darwin/build-deps.sh), ahead
# of Homebrew's: it has Brotli and sends the HTTP/2 preface and the header
# order Safari sends. Its cached location is forgotten on every run, so a
# build directory first configured against Homebrew's libsoup moves over.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$root/WebKitBuild/darwin/prefix/lib/pkgconfig/libsoup-3.0.pc" ] || { echo "run WebKitBuild/darwin/build-deps.sh first" >&2; exit 1; }
# /opt/homebrew on arm64 Macs, /usr/local on Intel ones; keg-only formulas sit under opt/.
brew=$(brew --prefix)
exec cmake -S "$root" -B "$root/WebKitBuild/wpe-darwin" -G Ninja \
	-U 'Soup3_*' -U 'PC_Soup3*' -U 'pkgcfg_lib_PC_Soup3*' \
	-DPORT=WPE -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="$root/WebKitBuild/darwin/prefix;$brew;$(brew --prefix icu4c);$brew/opt/sqlite;$brew/opt/curl;$brew/opt/openssl@3" \
	-DENABLE_TOOLS=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_MINIBROWSER=OFF \
	-DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
	-DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
	-DUSE_ATK=OFF -DUSE_FLITE=OFF -DUSE_GBM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBDRM=OFF \
	-DUSE_LIBHYPHEN=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_LCMS=OFF \
	-DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_VIDEO=ON -DENABLE_WEB_AUDIO=ON -DUSE_GSTREAMER=ON -DUSE_GSTREAMER_GL=OFF \
	-DENABLE_MEDIA_STREAM=ON -DENABLE_MEDIA_RECORDER=ON -DENABLE_WEB_CODECS=ON -DENABLE_MEDIA_SOURCE=ON -DENABLE_MEDIA_SOURCE_IN_WORKERS=ON \
	-DENABLE_WEB_RTC=ON -DENABLE_AV1=ON -DENABLE_WEBGPU=ON \
	-DENABLE_SPEECH_SYNTHESIS=ON -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=ON -DENABLE_WEB_AUTHN=ON \
	-DENABLE_WEBDRIVER=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_PDFJS=ON -DENABLE_THUNDER=OFF \
	-DENABLE_WEBXR=OFF -DENABLE_ENCRYPTED_MEDIA=ON -DENABLE_LEGACY_ENCRYPTED_MEDIA=ON -DENABLE_WEBGL=ON \
	-DENABLE_POINTER_LOCK=ON \
	-DENABLE_PICTURE_IN_PICTURE_API=ON -DENABLE_VIDEO_PRESENTATION_MODE=ON -DENABLE_WIRELESS_PLAYBACK_TARGET=ON \
	-DENABLE_DEVICE_ORIENTATION=ON -DENABLE_MAC_GESTURE_EVENTS=ON \
	-DENABLE_AUTOCAPITALIZE=OFF \
	-DENABLE_MODEL_ELEMENT=ON \
	-DENABLE_NAVIGATOR_STANDALONE=ON \
	"$@"
