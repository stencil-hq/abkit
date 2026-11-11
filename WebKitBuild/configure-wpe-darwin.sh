#!/bin/sh
# Configures the WPE port for a macOS host with every Linux-only piece off.
# Usage: configure-wpe-darwin.sh [extra cmake args]
#
# Off for the probe, each needing port work of its own on Darwin:
# - media (ENABLE_VIDEO, WEB_AUDIO, USE_GSTREAMER): GStreamer is the GLib
#   ports' only media backend;
# - WebGL: the GLib ports hand ANGLE's texture to the compositor through
#   libepoxy on the system's GL, which Darwin lacks (the Win port's
#   ANGLE-on-ANGLE path is the model);
# - the GPU process and DMA-BUF frames: frames cross as shared memory.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
exec cmake -S "$root" -B "$root/WebKitBuild/wpe-darwin" -G Ninja \
	-DPORT=WPE -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH="/opt/homebrew;/opt/homebrew/opt/icu4c@79;/opt/homebrew/opt/sqlite;/opt/homebrew/opt/curl;/opt/homebrew/opt/openssl@3" \
	-DENABLE_TOOLS=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_MINIBROWSER=OFF \
	-DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
	-DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
	-DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
	-DUSE_ATK=OFF -DUSE_FLITE=OFF -DUSE_GBM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBDRM=OFF \
	-DUSE_LIBHYPHEN=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_LCMS=OFF \
	-DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DUSE_GSTREAMER=OFF \
	-DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF -DENABLE_WEB_CODECS=OFF \
	-DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_GAMEPAD=OFF \
	-DENABLE_WEBDRIVER=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_PDFJS=OFF -DENABLE_THUNDER=OFF \
	-DENABLE_WEBXR=OFF -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_MEDIA_SOURCE=OFF -DENABLE_WEBGL=OFF \
	"$@"
