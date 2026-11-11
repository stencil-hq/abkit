#!/bin/sh
# Gathers a Linux WPE build into one relocatable engine directory: the
# library, its helper processes and the injected bundle side by side, where
# the library looks for them (directoryOfWebKitLibrary()). The system
# provides the other libraries, as for any WPE build.
# Usage: package-engine-linux.sh BUILD_DIR OUT_DIR
set -e
build=$1
out=$2
[ -d "$build/lib" ] && [ -n "$out" ] || { echo "usage: $0 BUILD_DIR OUT_DIR" >&2; exit 2; }
rm -rf "$out"
mkdir -p "$out"
# The soname is what embedders and the helpers load.
cp -L "$build/lib/libWPEWebKit-2.0.so.1" "$out/libWPEWebKit-2.0.so.1"
cp "$build/bin/WPEWebProcess" "$build/bin/WPENetworkProcess" "$build/lib/libWPEInjectedBundle.so" "$out/"
strip --strip-unneeded "$out/libWPEWebKit-2.0.so.1"
for file in WPEWebProcess WPENetworkProcess libWPEInjectedBundle.so; do
	# The helpers find the library beside them, wherever the directory is.
	patchelf --set-rpath '$ORIGIN' "$out/$file"
done
du -sh "$out"
ls -l "$out"
