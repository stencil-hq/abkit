#!/bin/sh
# Gathers a Linux WPE build into one relocatable engine directory: the
# library, its helper processes and the injected bundle side by side, where
# the library looks for them (directoryOfWebKitLibrary()), and ABHost, the
# program that runs the engine as a browser for a parent process, with its web
# process extension where it looks for it (extensions/).
# The TLS backend linux/build-deps.sh built goes beside the library, where the
# network process loads it from (BundledTLSBackend.cpp), and the remote
# inspector's frontend too, where the engine looks for it when there is no
# install prefix.
# Everything else the engine loads comes along, so the package runs on a
# machine that has nothing but the C library (and, with glibc, libstdc++):
# - every shared library any of its ELF files needs, in lib/, found through
#   $ORIGIN run paths;
# - the EGL and GLES the engine draws with, Mesa's on llvmpipe, which libepoxy
#   opens by name from lib/;
# - GStreamer's plugins and the scanner that reads them, in lib/gstreamer-1.0;
# - fontconfig's configuration in lib/fonts, with DejaVu as a last resort for a
#   machine that has no fonts at all; xkeyboard-config's keymaps in lib/xkb;
#   ICU's data in lib/icu where the distribution keeps it in a file (Alpine);
# - an empty lib/gio/modules, so GIO loads no module of the build's prefix.
# The engine points the libraries that look in absolute paths at these
# (useBundledRuntimePathsIfPresent()).
# Usage: package-engine-linux.sh BUILD_DIR OUT_DIR DEPS_PREFIX
#   DEPS_PREFIX   linux/build-deps.sh's prefix (libsoup, GLib, the TLS module, Mesa)
set -e
build=$1
out=$2
deps=$3
[ -d "$build/lib" ] && [ -n "$out" ] && [ -d "$deps/lib" ] || { echo "usage: $0 BUILD_DIR OUT_DIR DEPS_PREFIX" >&2; exit 2; }
build=$(cd "$build" && pwd)
deps=$(cd "$deps" && pwd)
rm -rf "$out"
mkdir -p "$out/extensions"
out=$(cd "$out" && pwd)
lib=$out/lib
mkdir -p "$lib/gio/modules"

library=libWPEWebKit-2.0.so.1
cp -L "$build/lib/$library" "$out/$library"
cp "$build/bin/ABWebProcess" "$build/bin/ABNetworkProcess" "$build/bin/ABHost" "$build/lib/libWPEInjectedBundle.so" "$out/"
cp "$build/bin/extensions/libABHostExtension.so" "$out/extensions/"
cp "$deps/lib/gio/modules/libgioboringssl.so" "$out/"
cp "$build/share/inspector.gresource" "$out/"
# libepoxy opens these by name rather than linking them.
cp -L "$deps/lib/libEGL.so.1" "$deps/lib/libGLESv2.so.2" "$lib/"

# GStreamer finds its plugins by absolute path, so the plugin directory and
# the scanner that reads them come along.
gst_plugins=$(pkg-config --variable=pluginsdir gstreamer-1.0)
gst_scanner=$(pkg-config --variable=pluginscannerdir gstreamer-1.0)
# Debian's pkg-config file names a directory its package does not install the
# scanner in.
[ -x "$gst_scanner/gst-plugin-scanner" ] || gst_scanner=$(dirname "$(find /usr/lib /usr/libexec -name gst-plugin-scanner -type f | head -n 1)")
mkdir -p "$lib/gstreamer-1.0"
for plugin in "$gst_plugins"/*.so; do
	case $(basename "$plugin") in
	# Sinks and sources of desktop toolkits, display servers and GL, hardware
	# codec and capture APIs whose drivers a package cannot bring, and the
	# Python loader: the engine renders and plays through none of them, and
	# they would bring GTK, X11, Wayland, Vulkan and Python into the package.
	libgstgtk.so|libgstgtk4.so|libgstpython.so|libgstximagesink.so|libgstximagesrc.so|libgstrfbsrc.so|libgstopengl.so|libgstqml6.so|\
	libgstqmlgl.so|libgstqt5.so|libgstwaylandsink.so|libgstvulkan.so|libgstva.so|libgstvaapi.so|libgstnvcodec.so|libgstmsdk.so|\
	libgstkms.so|libgstsdl.so|libgstsdlelem.so|libgstcacasink.so|libgstaasink.so|libgstdirectfb.so|libgstx11.so|libgstxvimagesink.so|\
	libgstdecklink.so|libgstdvb.so|libgstdc1394.so|libgstopencv.so|libgstopenni2.so|libgstladspa.so|libgstlv2.so|libgstfluidsynthmidi.so|\
	libgstwildmidi.so|libgstopenal.so|libgstjack.so|libgstpipewire.so|libgstbluez.so|libgstshm.so|libgstipcpipeline.so|libgstgs.so|libgstqroverlay.so|\
	libgstzbar.so|libgstmicrodns.so|libgstteletext.so|libgstcolormanagement.so|libgstresindvd.so|libgstdvdread.so|libgstcdio.so|libgstcdparanoia.so|\
	libgstnice.so|libgstsrt.so|libgstrtmp.so|libgstrtmp2.so|libgstcurl.so|libgstsoup.so|libgstadaptivedemux2.so|libgstopenexr.so|libgstrsvg.so|\
	libgstchromaprint.so|libgstspandsp.so|libgstsoundtouch.so|libgstbs2b.so|libgstmodplug.so|libgstopenmpt.so|libgstgme.so|libgstkate.so|libgstzxing.so|\
	libgstassrender.so|libgstttmlsubs.so|libgstfaac.so|libgstneonhttpsrc.so|libgstmplex.so|libgstmpeg2enc.so|libgstsmoothstreaming.so|libgsthls.so|libgstdash.so|\
	libgstwebrtc.so|libgstwebrtcdsp.so|libgstdtls.so|libgstsrtp.so|libgstsctp.so|libgstaom.so|libgstsvthevcenc.so|libgstx265.so|libgstx264.so|libgstopenh264.so|\
	libgstpulseaudio.so|libgstoss4.so|libgstossaudio.so|libgstv4l2codecs.so|libgstuvch264.so|libgstuvcgadget.so|libgstcamerabin.so|libgstgdkpixbuf.so|libgstcairo.so|\
	libgstpango.so|libgstmse.so|libgstanalyticsoverlay.so|libgstonnx.so|libgstwpe.so|libgstwpe2.so|libgstaes.so|libgstsvtav1.so|libgstqsv.so|libgstajasrc.so|libgstaja.so) ;;
	*) cp "$plugin" "$lib/gstreamer-1.0/" ;;
	esac
done
cp "$gst_scanner/gst-plugin-scanner" "$lib/gstreamer-1.0/gst-plugin-scanner"

# fontconfig reads fonts.conf, and the conf.d it includes, from the prefix it
# was built in, so the package carries that configuration, and DejaVu, which a
# machine without fonts falls back on.
# The distribution's, with the rules its packages add in conf.d.
font_config=/etc/fonts
mkdir -p "$lib/fonts/conf.d" "$lib/fonts/dejavu"
cp "$font_config/fonts.conf" "$lib/fonts/"
for conf in "$font_config"/conf.d/*.conf; do
	cp -L "$conf" "$lib/fonts/conf.d/" 2>/dev/null || true
done
for font in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSerif.ttf DejaVuSerif-Bold.ttf DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf; do
	path=$(find /usr/share/fonts -name "$font" | head -n 1)
	[ -z "$path" ] || cp "$path" "$lib/fonts/dejavu/"
done
cat > "$lib/fonts/conf.d/99-abkit-bundled-fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<!-- The fonts the engine brings, after the machine's own. -->
<fontconfig>
	<dir prefix="relative">../dejavu</dir>
</fontconfig>
EOF

# Alpine's libicudata is a stub; the data is a file ICU opens by absolute path.
for data in /usr/share/icu/*/icudt*.dat; do
	if [ -f "$data" ]; then
		mkdir -p "$lib/icu"
		cp "$data" "$lib/icu/"
	fi
done

# xkbcommon compiles keymaps from the xkeyboard-config of the build machine.
xkb=$(pkg-config --variable=xkb_base xkeyboard-config 2>/dev/null || true)
[ -d "$xkb" ] || xkb=/usr/share/X11/xkb
mkdir -p "$lib/xkb"
for part in compat geometry keycodes rules symbols types; do
	cp -RL "$xkb/$part" "$lib/xkb/"
done

is_elf() {
	[ "$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n')" = 7f454c46 ]
}

# The C library, and with glibc the C++ runtime every distribution has a newer
# copy of, are the system's; everything else is packed.
is_system_library() {
	case $1 in
	ld-linux*|ld-musl*|libc.so.*|libc.musl*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libanl.so.*|libmvec.so.*|libBrokenLocale.so.*|libnss_*|linux-vdso.so.*|linux-gate.so.*) return 0 ;;
	libstdc++.so.*|libgcc_s.so.*) [ "$libc" = glibc ] ;;
	*) return 1 ;;
	esac
}
case $(ldd --version 2>&1 | head -n 1) in
*musl*) libc=musl ;;
*) libc=glibc ;;
esac

# Dependencies resolve to the deps prefix first (its GLib and libsoup are
# newer than the system's), then to the build tree's libraries.
LD_LIBRARY_PATH="$out:$lib:$deps/lib:$build/lib"
export LD_LIBRARY_PATH
queue=$(mktemp)
next=$(mktemp)
# What was built here rather than taken from the distribution, which ships its
# libraries stripped already.
own=$(mktemp)
trap 'rm -f "$queue" "$next" "$own"' EXIT
printf '%s\n' "$lib/libEGL.so.1" "$lib/libGLESv2.so.2" > "$own"
find "$out" -type f > "$queue"
while [ -s "$queue" ]; do
	: > "$next"
	while IFS= read -r file; do
		is_elf "$file" || continue
		ldd "$file" 2>/dev/null | awk '$2 == "=>" { print $1, $3 }' | while read -r name path; do
			is_system_library "$name" && continue
			if [ "$path" = "not" ]; then
				echo "package-engine-linux: $file needs $name, which is nowhere" >&2
				exit 1
			fi
			case $path in "$out"/*) continue ;; esac
			# The library itself, found by a build tree run path, is already beside the helpers.
			[ -e "$out/$name" ] || [ -e "$lib/$name" ] && continue
			cp -L "$path" "$lib/$name"
			case $path in "$deps"/*|"$build"/*) printf '%s\n' "$lib/$name" >> "$own" ;; esac
			chmod u+w "$lib/$name"
			printf '%s\n' "$lib/$name" >> "$next"
		done
	done < "$queue"
	cp "$next" "$queue"
done

# What was built here is stripped (the distribution's libraries come stripped),
# and then each ELF finds its dependencies in lib/, by a path relative to
# itself. strip and patchelf must not run on the libraries they are rewriting
# (libz, and with musl libstdc++), so the search path above is dropped first.
unset LD_LIBRARY_PATH
find "$out" -type f | while IFS= read -r file; do
	is_elf "$file" || continue
	chmod u+w "$file"
	case ${file#"$out"/} in
	lib/*) grep -qxF "$file" "$own" && strip --strip-unneeded "$file" ;;
	*) strip --strip-unneeded "$file" ;;
	esac
	case ${file#"$out"/} in
	lib/gstreamer-1.0/*) rpath='$ORIGIN/..' ;;
	lib/*) rpath='$ORIGIN' ;;
	extensions/*) rpath='$ORIGIN/..:$ORIGIN/../lib' ;;
	*) rpath='$ORIGIN:$ORIGIN/lib' ;;
	esac
	patchelf --set-rpath "$rpath" "$file"
done

# A check that nothing is left that only this machine has, and that nothing
# asks the C library for more than the oldest one it is to run on (this one)
# has: ldd names both.
missing=$(find "$out" -type f | while IFS= read -r file; do
	is_elf "$file" || continue
	ldd "$file" 2>&1 | grep "not found" | sed "s|^|$file: |"
done)
[ -z "$missing" ] || { echo "package-engine-linux: unresolved after packing:" >&2; echo "$missing" >&2; exit 1; }
du -sh "$out"
ls -l "$out"
