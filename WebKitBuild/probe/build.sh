#!/bin/sh
# Builds the offscreen probe against the WPE build in WebKitBuild/wpe-darwin.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$here/../wpe-darwin
source=$here/../../Source
exec cc -o "$here/probe" "$here/probe.c" \
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
	-Wl,-rpath,"$build/lib"
