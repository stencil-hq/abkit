#!/bin/sh
# Gathers a WPE build into one relocatable engine directory: the library,
# its helper processes and the injected bundle side by side, where the
# library looks for them (directoryOfWebKitLibrary()), and WPEAgentHost,
# the program that runs the engine as a browser for a parent process, with
# its web process extension where it looks for it (extensions/).
# Usage: package-engine.sh BUILD_DIR OUT_DIR
set -e
build=$1
out=$2
[ -d "$build/lib" ] && [ -n "$out" ] || { echo "usage: $0 BUILD_DIR OUT_DIR" >&2; exit 2; }
rm -rf "$out"
mkdir -p "$out/extensions"
library=libWPEWebKit-2.0.1.dylib
cp "$build/lib/$library" "$out/$library"
cp "$build/bin/WPEWebProcess" "$build/bin/WPENetworkProcess" "$build/bin/WPEAgentHost" "$build/lib/libWPEInjectedBundle.so" "$out/"
cp "$build/bin/extensions/libWPEAgentHostExtension.so" "$out/extensions/"
strip -x "$out/$library"
dependents="WPEWebProcess WPENetworkProcess WPEAgentHost libWPEInjectedBundle.so extensions/libWPEAgentHostExtension.so"
for file in $dependents; do
	# Each finds the library in the engine directory, wherever that is.
	for rpath in $(otool -l "$out/$file" | awk '/LC_RPATH/ { getline; getline; print $2 }'); do
		install_name_tool -delete_rpath "$rpath" "$out/$file"
	done
	case $file in
	*/*) install_name_tool -add_rpath @loader_path/.. "$out/$file" ;;
	*) install_name_tool -add_rpath @loader_path "$out/$file" ;;
	esac
done
for file in "$library" $dependents; do
	codesign --force --sign - "$out/$file" 2>/dev/null
done
du -sh "$out"
ls -l "$out"
