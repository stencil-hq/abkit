#!/bin/sh
# Builds the offscreen probe for Windows (probe.exe) against the WPE build
# in WebKitBuild/wpe-win, with the toolchain that build uses.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$here/../wpe-win
source=$here/../../Source
sdk=$here/../../WebKitLibraries/windows
deps=$here/../win/sysroot/clang64
# clang-cl reads an absolute path as a flag: compile from here.
cd "$here"
exec /opt/homebrew/opt/llvm/bin/clang-cl --target=x86_64-pc-windows-msvc /nologo /MD /O1 -fuse-ld=lld-link \
	-Wno-unused-command-line-argument \
	-imsvc "$sdk/crt/include" -imsvc "$sdk/sdk/include/ucrt" -imsvc "$sdk/sdk/include/um" -imsvc "$sdk/sdk/include/shared" \
	-I"$build/DerivedSources/WebKit" \
	-I"$build/DerivedSources/ForwardingHeaders/wpe" \
	-I"$build/DerivedSources/ForwardingHeaders/wpe-jsc" \
	-I"$build/DerivedSources/WPEPlatform" \
	-I"$source/WebKit/WPEPlatform" \
	-I"$build/JavaScriptCoreGLib/DerivedSources" \
	-I"$build/JavaScriptCoreGLib/Headers" \
	-I"$build" \
	-I"$deps/include/glib-2.0" -I"$deps/lib/glib-2.0/include" -I"$deps/include/libsoup-3.0" -I"$deps/include" \
	/Feprobe.exe /Foprobe.obj probe.c \
	/link -libpath:"$sdk/crt/lib/x64" -libpath:"$sdk/sdk/lib/ucrt/x64" -libpath:"$sdk/sdk/lib/um/x64" \
	-libpath:"$build/lib" -libpath:"$deps/lib" \
	WPEWebKit-2.0.lib glib-2.0.lib gobject-2.0.lib gio-2.0.lib
