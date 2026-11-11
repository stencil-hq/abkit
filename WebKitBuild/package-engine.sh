#!/bin/sh
# Gathers a WPE build into one relocatable engine directory: the library,
# its helper processes and the injected bundle side by side, where the
# library looks for them (directoryOfWebKitLibrary()).
# Usage: package-engine.sh BUILD_DIR OUT_DIR
set -e
build=$1
out=$2
[ -d "$build/lib" ] && [ -n "$out" ] || { echo "usage: $0 BUILD_DIR OUT_DIR" >&2; exit 2; }
rm -rf "$out"
mkdir -p "$out"
library=libWPEWebKit-2.0.1.dylib
cp "$build/lib/$library" "$out/$library"
cp "$build/bin/WPEWebProcess" "$build/bin/WPENetworkProcess" "$build/lib/libWPEInjectedBundle.so" "$out/"
strip -x "$out/$library"
for file in WPEWebProcess WPENetworkProcess libWPEInjectedBundle.so; do
	# The helpers find the library beside them, wherever the directory is.
	for rpath in $(otool -l "$out/$file" | awk '/LC_RPATH/ { getline; getline; print $2 }'); do
		install_name_tool -delete_rpath "$rpath" "$out/$file"
	done
	install_name_tool -add_rpath @loader_path "$out/$file"
done
for file in "$library" WPEWebProcess WPENetworkProcess libWPEInjectedBundle.so; do
	codesign --force --sign - "$out/$file" 2>/dev/null
done
du -sh "$out"
ls -l "$out"
