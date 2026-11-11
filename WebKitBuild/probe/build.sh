#!/bin/sh
# Builds the offscreen probe against a WPE build of this host: the one in
# WebKitBuild/wpe-darwin, or the directory given (WebKitBuild/wpe-linux).
# Usage: build.sh [BUILD_DIR]
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/../wpe-darwin}
source=$here/../../Source
exec cc $PROBE_CFLAGS -o "$here/probe" "$here/probe.c" \
	-I"$build/DerivedSources/WebKit" \
	-I"$build/DerivedSources/ForwardingHeaders/wpe" \
	-I"$build/DerivedSources/ForwardingHeaders/wpe-jsc" \
	-I"$build/DerivedSources/WPEPlatform" \
	-I"$source/WebKit/WPEPlatform" \
	-I"$build/JavaScriptCoreGLib/DerivedSources" \
	-I"$build/JavaScriptCoreGLib/Headers" \
	-I"$build" \
	$(pkg-config --cflags glib-2.0 gio-2.0 libsoup-3.0 xkbcommon) \
	-L"$build/lib" -lWPEWebKit-2.0 \
	$(pkg-config --libs glib-2.0 gobject-2.0 gio-2.0) \
	$(test "$(uname)" = Darwin && echo -framework IOSurface) \
	-Wl,-rpath,"$build/lib"
