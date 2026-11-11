#!/bin/sh
# Configures the WPE port for x86_64 Windows, cross-compiled from macOS with
# clang-cl (see win/toolchain.cmake for where the SDK and libraries come from).
# Usage: configure-wpe-win.sh [extra cmake args]
#
# Off as on Darwin (configure-wpe-darwin.sh), each needing port work of its
# own: media, WebGL, the GPU process, DMA-BUF frames.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
deps="$root/WebKitBuild/win/sysroot/clang64"
# The packages' .pc files say prefix=/clang64.
export PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig:$deps/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$root/WebKitBuild/win/sysroot"
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"
exec cmake -S "$root" -B "$root/WebKitBuild/wpe-win" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$root/WebKitBuild/win/toolchain.cmake" \
	-DPORT=WPE -DCMAKE_BUILD_TYPE=Release \
	-DENABLE_TOOLS=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_MINIBROWSER=OFF \
	-DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
	-DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
	-DUSE_ATK=OFF -DUSE_FLITE=OFF -DUSE_GBM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBDRM=OFF \
	-DUSE_LIBHYPHEN=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_LCMS=OFF -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF \
	-DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DUSE_GSTREAMER=OFF \
	-DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF -DENABLE_WEB_CODECS=OFF \
	-DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=OFF \
	-DENABLE_WEBDRIVER=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_PDFJS=OFF -DENABLE_THUNDER=OFF \
	-DENABLE_WEBXR=OFF -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_MEDIA_SOURCE=OFF -DENABLE_WEBGL=OFF \
	"$@"
