#!/bin/sh
# Gathers a WPE build into one relocatable engine directory: the library,
# its helper processes and the injected bundle side by side, where the
# library looks for them (directoryOfWebKitLibrary()), and ABHost,
# the program that runs the engine as a browser for a parent process, with
# its web process extension where it looks for it (extensions/).
# The libsoup and the TLS backend that darwin/build-deps.sh built go in too:
# the library links that libsoup, and the network process loads the backend
# from beside the library.
# The remote inspector's frontend goes beside the library too, where the
# engine looks for it when there is no install prefix to find it in.
# Everything else the engine loads by absolute path, from Homebrew or from the
# build tree, comes along, so the package runs on a machine that has none of it:
# the dependencies of every Mach-O in it are copied into lib/ and rewired to
# point at each other there, GStreamer's plugins and the scanner that reads them
# go in lib/gstreamer-1.0, and the fontconfig configuration the fonts code reads
# goes in lib/fonts. Runtime lookups of the last two go through
# useBundledRuntimePathsIfPresent().
# Usage: package-engine.sh BUILD_DIR OUT_DIR
set -e
build=$1
out=$2
[ -d "$build/lib" ] && [ -n "$out" ] || { echo "usage: $0 BUILD_DIR OUT_DIR" >&2; exit 2; }
rm -rf "$out"
mkdir -p "$out/extensions"
out=$(cd "$out" && pwd -P)
library=libWPEWebKit-2.0.1.dylib
cp "$build/lib/$library" "$out/$library"
cp "$build/bin/ABWebProcess" "$build/bin/ABNetworkProcess" "$build/bin/ABHost" "$build/lib/libWPEInjectedBundle.so" "$out/"
cp "$build/bin/extensions/libABHostExtension.so" "$out/extensions/"
# The viewer shows the host's pages in windows; clients look for it beside the host.
cp "$build/bin/ABView" "$out/"
strip -x "$out/$library"
deps=$(cd "$(dirname "$0")" && pwd)/darwin/prefix
soup=libsoup-3.0.0.dylib
cp "$deps/lib/$soup" "$deps/lib/gio/modules/libgioboringssl.so" "$out/"
# WebRTC is a library of its own, which the engine loads from beside itself.
webrtc=libwebrtc.dylib
cp "$build/lib/$webrtc" "$out/"
# The remote inspector's frontend, which the engine loads from beside the library.
cp "$build/share/inspector.gresource" "$out/"
linked_soup=$(otool -L "$out/$library" | awk '/libsoup-3\.0/ { print $1 }')
install_name_tool -id "@rpath/$soup" "$out/$soup"
install_name_tool -change "$linked_soup" "@loader_path/$soup" "$out/$library"
dependents="ABWebProcess ABNetworkProcess ABHost libWPEInjectedBundle.so extensions/libABHostExtension.so"
for file in $dependents; do
	# Each finds the library in the engine directory, wherever that is.
	for rpath in $(otool -l "$out/$file" | awk '/LC_RPATH/ { getline; getline; print $2 }'); do
		install_name_tool -delete_rpath "$rpath" "$out/$file"
	done
	case $file in
	*/*) install_name_tool -add_rpath @loader_path/.. "$out/$file" ;;
	*) install_name_tool -add_rpath @loader_path "$out/$file" ;;
	esac
	install_name_tool -change "$linked_soup" "@rpath/$soup" "$out/$file"
done
# GStreamer finds its plugins by absolute path, so the plugin directory, the
# scanner that reads them and the libraries they use come along too.
lib=$out/lib
mkdir -p "$lib"
gst_plugins=$(pkg-config --variable=pluginsdir gstreamer-1.0 2>/dev/null || true)
if [ -n "$gst_plugins" ] && [ -d "$gst_plugins" ]; then
	mkdir -p "$lib/gstreamer-1.0"
	for plugin in "$gst_plugins"/*.dylib "$gst_plugins"/*.so; do
		[ -e "$plugin" ] || continue
		case $(basename "$plugin") in
		# Sinks and sources of desktop toolkits, X11 and GL, and the Python
		# loader: the engine renders and plays through none of them, and they
		# would bring GTK 3 and 4, X11 and Python into the package.
		libgstgtk.*|libgstgtk4.*|libgstpython.*|libgstximagesink.*|libgstximagesrc.*|libgstrfbsrc.*|libgstopengl.*|libgstqml6.*) ;;
		*) cp "$plugin" "$lib/gstreamer-1.0/" ;;
		esac
	done
	gst_scanner=$(pkg-config --variable=pluginscannerdir gstreamer-1.0 2>/dev/null || true)
	if [ -n "$gst_scanner" ] && [ -x "$gst_scanner/gst-plugin-scanner" ]; then
		cp "$gst_scanner/gst-plugin-scanner" "$lib/gstreamer-1.0/gst-plugin-scanner"
	fi
else
	echo "package-engine: pkg-config did not name a GStreamer plugin directory; the package will need GStreamer installed" >&2
fi

# GIO loads every module in the directory it was built with (Homebrew's, with
# glib-networking's GnuTLS backend linking Homebrew's GLib); the package's own,
# empty, is the one useBundledRuntimePathsIfPresent() points it at.
mkdir -p "$lib/gio/modules"
# fontconfig reads fonts.conf, and the conf.d it includes, from the prefix it
# was built in, so the package carries that configuration too.
font_config=$(pkg-config --variable=confdir fontconfig 2>/dev/null || true)
if [ -n "$font_config" ] && [ -f "$font_config/fonts.conf" ]; then
	mkdir -p "$lib/fonts"
	cp "$font_config/fonts.conf" "$lib/fonts/"
	if [ -d "$font_config/conf.d" ]; then
		cp -R "$font_config/conf.d" "$lib/fonts/"
	fi
else
	echo "package-engine: pkg-config did not name a fontconfig configuration directory; the package will need fontconfig installed" >&2
fi
chmod -R u+w "$lib"

# otool -h answers 0 for files that are no object file at all.
is_macho() {
	file -b "$1" | grep -q '^Mach-O'
}

# What a Mach-O links against, without its own install name. A fat binary names
# each dependency once per architecture, so the names are deduplicated.
load_dependencies() {
	otool -l "$1" | awk '/cmd LC_LOAD_DYLIB|cmd LC_LOAD_WEAK_DYLIB|cmd LC_REEXPORT_DYLIB|cmd LC_LOAD_UPWARD_DYLIB/ { getline; getline; print $2 }' | sort -u
}

file_rpaths() {
	otool -l "$1" | awk '/LC_RPATH/ { getline; getline; print $2 }' | sort -u
}

# A Homebrew path is usually a symlink into the Cellar: pack the real file, so
# the two spellings of one library are packed once.
resolve_path() {
	path=$1
	while [ -L "$path" ]; do
		target=$(readlink "$path")
		case $target in
		/*) path=$target ;;
		*) path=$(dirname "$path")/$target ;;
		esac
	done
	(cd "$(dirname "$path")" && printf '%s/%s\n' "$(pwd -P)" "$(basename "$path")")
}

# The rpath that reaches $out/lib from the directory holding $1.
rpath_for() {
	relative=${1#"$out"/}
	directory=$(dirname "$relative")
	if [ "$directory" = "." ]; then
		directory=
	elif [ "$directory" = lib ]; then
		echo @loader_path
		return
	fi
	rpath=@loader_path
	for component in $(printf '%s' "$directory" | tr / ' '); do
		rpath="$rpath/.."
	done
	echo "$rpath/lib"
}

has_rpath() {
	file_rpaths "$1" | grep -qxF "$2"
}

# The file a @rpath/$2 reference names, looked for along the rpaths $1 carries
# now and in the prefixes the package is being filled from.
resolve_rpath_reference() {
	for directory in $(file_rpaths "$1") $(cat "$search" 2>/dev/null); do
		case $directory in
		@loader_path) directory=$(dirname "$1") ;;
		@loader_path/*) directory="$(dirname "$1")/${directory#@loader_path/}" ;;
		@*) continue ;;
		esac
		if [ -e "$directory/$2" ]; then
			printf '%s\n' "$directory/$2"
			return
		fi
	done
}

# Copies a real file into the package; $1 the file, $2 where it goes, $3 the
# prefix it came from, which later references may be resolved in.
pack_file() {
	cp -f "$1" "$2"
	chmod u+w "$2"
	printf '%s\n' "$2" >> "$next"
	printf '%s\n' "$3" >> "$search"
}

map=$(mktemp)
queue=$(mktemp)
next=$(mktemp)
search=$(mktemp)
trap 'rm -f "$map" "$queue" "$next" "$search"' EXIT
if [ -n "$gst_plugins" ]; then
	# The plugins name the GStreamer libraries by @rpath, which only their own
	# prefix can resolve.
	printf '%s\n' "$gst_plugins/.." > "$search"
fi

find "$out" -type f > "$queue"
while [ -s "$queue" ]; do
	: > "$next"
	while IFS= read -r file; do
		is_macho "$file" || continue

		# An install name that still spells out a prefix is a reference to it.
		install_name=$(otool -D "$file" 2>/dev/null | tail -n +2 | head -n 1)
		case $install_name in
		/usr/lib/*|/System/*) ;;
		/*) install_name_tool -id "@rpath/$(basename "$file")" "$file" ;;
		esac

		for dependency in $(load_dependencies "$file"); do
			case $dependency in
			"$out"/*) continue ;;
			/usr/lib/*|/System/*) continue ;;
			@loader_path/*)
				name=${dependency#@loader_path/}
				source="$(dirname "$file")/$name"
				if [ ! -e "$source" ]; then
					source=$(resolve_rpath_reference "$file" "$name")
				fi
				;;
			@rpath/*)
				name=${dependency#@rpath/}
				source=$(resolve_rpath_reference "$file" "$name")
				;;
			/*)
				name=$(basename "$dependency")
				source=$dependency
				;;
			*) continue ;;
			esac
			if [ -z "$source" ]; then
				echo "package-engine: cannot find what $dependency names for $file" >&2
				continue
			fi
			case $source in
			"$out"/*) continue ;;
			esac
			if [ ! -e "$source" ]; then
				echo "package-engine: missing $source, a dependency of $file" >&2
				continue
			fi
			real=$(resolve_path "$source")
			packed=$(awk -v key="$real" '$1 == key { print $2; exit }' "$map")
			if [ -z "$packed" ]; then
				packed=$name
				printf '%s %s\n' "$real" "$packed" >> "$map"
				pack_file "$real" "$lib/$packed" "$(dirname "$source")"
			fi
			if [ "$dependency" != "@rpath/$packed" ]; then
				install_name_tool -change "$dependency" "@rpath/$packed" "$file"
			fi
		done

		# An rpath into a prefix the package does not have is worth nothing.
		for directory in $(file_rpaths "$file"); do
			case $directory in
			/usr/lib/*|/System/*) ;;
			/*) install_name_tool -delete_rpath "$directory" "$file" ;;
			esac
		done
		rpath=$(rpath_for "$file")
		has_rpath "$file" "$rpath" || install_name_tool -add_rpath "$rpath" "$file"
	done < "$queue"
	cp "$next" "$queue"
done

# Every edit above invalidated the ad hoc signature.
find "$out" -type f | while IFS= read -r file; do
	if is_macho "$file"; then
		codesign --force --sign - "$file" 2>/dev/null || true
	fi
done
du -sh "$out"
ls -l "$out"
