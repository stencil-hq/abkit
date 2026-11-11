#!/bin/sh
# Configures the WPE port for Windows on x64 or arm64, cross-compiled from a
# Linux or macOS host with clang-cl (see win/toolchain.cmake for where the SDK
# and libraries come from; win/fetch-deps.py puts them there), into
# WebKitBuild/wpe-win-ARCH.
# Usage: configure-wpe-win.sh x64|arm64 [extra cmake args]
# WEBKIT_LLVM_BIN names the directory of clang-cl and the other LLVM tools when
# they are not on PATH under their plain names.
#
# On, as on macOS (configure-wpe-darwin.sh): the PDF.js viewer, pointer lock,
# the DeviceMotion, DeviceOrientation and GestureEvent interfaces (no events
# fire: the headless host has no sensors and sends no gestures), the <model>
# element's interface (no player: its ready promise rejects), and
# navigator.standalone; autocapitalize is off as it is there.
# Off, each needing port work of its own on Windows: media (GStreamer, and with
# it Web Audio, MediaStream, WebRTC, MSE, WebCodecs, Encrypted Media), WebGL and
# WebGPU (ANGLE's D3D backend is not wired to the WPE platform), the GPU
# process, gamepads, WebAuthn, speech (no Windows synthesizer backend), and
# DMA-BUF frames.
set -e
arch=$1
case $arch in
x64|arm64) shift ;;
*) echo "usage: $0 x64|arm64 [extra cmake args]" >&2; exit 2 ;;
esac
root=$(cd "$(dirname "$0")/.." && pwd)
case $arch in
x64) environment=clang64 ;;
arm64) environment=clangarm64 ;;
esac
deps="$root/WebKitBuild/win/sysroot/$environment"
[ -f "$deps/lib/pkgconfig/glib-2.0.pc" ] || { echo "run WebKitBuild/win/fetch-deps.py $arch first" >&2; exit 1; }
# The packages' .pc files say prefix=/clang64 (or /clangarm64).
export PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig:$deps/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$root/WebKitBuild/win/sysroot"
export WEBKIT_WIN_ARCH=$arch
exec cmake -S "$root" -B "$root/WebKitBuild/wpe-win-$arch" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$root/WebKitBuild/win/toolchain.cmake" \
	-DWEBKIT_WIN_ARCH="$arch" ${WEBKIT_LLVM_BIN:+-DWEBKIT_LLVM_BIN="$WEBKIT_LLVM_BIN"} \
	-DPORT=WPE -DCMAKE_BUILD_TYPE=Release \
	-DENABLE_TOOLS=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_MINIBROWSER=OFF \
	-DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
	-DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
	-DUSE_ATK=OFF -DUSE_FLITE=OFF -DUSE_GBM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBDRM=OFF \
	-DUSE_LIBHYPHEN=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_LCMS=OFF -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF \
	-DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DUSE_GSTREAMER=OFF \
	-DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF -DENABLE_WEB_CODECS=OFF \
	-DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=OFF -DENABLE_WEB_AUTHN=OFF \
	-DENABLE_WEBDRIVER=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_PDFJS=ON -DENABLE_THUNDER=OFF \
	-DENABLE_WEBXR=OFF -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_MEDIA_SOURCE=OFF -DENABLE_WEBGL=OFF \
	-DENABLE_POINTER_LOCK=ON -DENABLE_DEVICE_ORIENTATION=ON -DENABLE_MAC_GESTURE_EVENTS=ON \
	-DENABLE_AUTOCAPITALIZE=OFF -DENABLE_MODEL_ELEMENT=ON -DENABLE_NAVIGATOR_STANDALONE=ON \
	"$@"
