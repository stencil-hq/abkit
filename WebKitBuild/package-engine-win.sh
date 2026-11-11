#!/bin/sh
# Gathers a Windows WPE build into one relocatable engine directory: the
# library, its helper processes and the injected bundle side by side (where
# the library looks for them), with every DLL they import from the build
# and from the MSYS2 sysroot, GIO's TLS modules and fontconfig's
# configuration.
# Usage: package-engine-win.sh BUILD_DIR OUT_DIR
set -e
build=$1
out=$2
[ -d "$build/bin" ] && [ -n "$out" ] || { echo "usage: $0 BUILD_DIR OUT_DIR" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
deps=$here/win/sysroot/clang64
objdump=/opt/homebrew/opt/llvm/bin/llvm-objdump

rm -rf "$out"
mkdir -p "$out/lib/gio/modules" "$out/etc"
cp "$build/bin/WPEWebKit-2.0.dll" "$build/bin/WPEWebProcess.exe" "$build/bin/WPENetworkProcess.exe" "$out/"
# The injected bundle is a module, wherever the generator put it.
find "$build/bin" "$build/lib" -maxdepth 1 -name '*WPEInjectedBundle*.dll' -exec cp {} "$out/" \;
# TLS for libsoup (OpenSSL's, over the Windows certificate store): GIO looks
# for modules under lib/gio/modules of the folder its own DLL is in.
cp "$deps/lib/gio/modules/libgioopenssl.dll" "$out/lib/gio/modules/"
# Where fontconfig looks from its own DLL: the system's fonts, per fonts.conf.
cp -R "$deps/etc/fonts" "$out/etc/fonts"

# Every DLL imported, transitively, that the build or the sysroot provides
# (the rest are Windows' own).
todo=$(ls "$out"/*.dll "$out"/*.exe "$out"/lib/gio/modules/*.dll)
while [ -n "$todo" ]; do
	next=
	for file in $todo; do
		for name in $("$objdump" -p "$file" | awk '/DLL Name:/ { print $3 }'); do
			[ -e "$out/$name" ] && continue
			for from in "$build/bin" "$deps/bin"; do
				if [ -e "$from/$name" ]; then
					cp "$from/$name" "$out/$name"
					next="$next $out/$name"
					break
				fi
			done
		done
	done
	todo=$next
done
du -sh "$out"
ls "$out" | wc -l
