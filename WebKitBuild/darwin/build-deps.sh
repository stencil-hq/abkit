#!/bin/sh
# Builds what the Darwin engine needs to look like Safari to a server and that Homebrew does not
# provide, into WebKitBuild/darwin/prefix:
# - libsoup with Brotli, with the HTTP/2 preface, the ALPN list and the request header order
#   Safari sends (patches/libsoup-safari.patch);
# - libgioboringssl.so, a GIO TLS backend on BoringSSL that sends Safari's ClientHello and asks the
#   Security framework for trust: glib-networking's OpenSSL backend
#   (patches/glib-networking-boringssl.patch) with the sources in Source/GIOBoringSSL, linked
#   statically against BoringSSL (patches/boringssl-safari-client-hello.patch);
# - soup-get, a client for checking the two without the engine.
# Sources are pinned by version and checksum, so a rebuild from nothing gives the same thing.
# Homebrew provides the rest: cmake ninja pkgconf python glib libnghttp2 brotli zstd libpsl sqlite.
#
# Usage: build-deps.sh [clean]
#   clean   removes the sources, build directories and prefix first (downloads are kept)
#
# Afterwards WebKitBuild/configure-wpe-darwin.sh finds libsoup in the prefix and
# WebKitBuild/package-engine.sh copies libsoup and the TLS module into the engine directory.
# To try them alone:
#   GIO_EXTRA_MODULES=prefix/lib/gio/modules GIO_USE_TLS=boringssl prefix/bin/soup-get https://tls.peet.ws/api/all
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
downloads=$here/downloads
sources=$here/src
build=$here/build
prefix=$here/prefix
patches=$here/patches

boringssl_commit=156c7b75ae9b8c3b3f847acf264f17594c3859fb
boringssl_url=https://github.com/google/boringssl/archive/$boringssl_commit.tar.gz
boringssl_sha256=4cbd70611e6400616fd5a58f42e19e7bafd568c674ecb3a6418c43ae1e3ac93b

glib_networking_version=2.90.1
glib_networking_url=https://download.gnome.org/sources/glib-networking/2.90/glib-networking-$glib_networking_version.tar.xz
glib_networking_sha256=ea95636590098355a05e3027579dba1a9a950b05e456b2488ea03d82e4b6c18a

libsoup_version=3.8.1
libsoup_url=https://download.gnome.org/sources/libsoup/3.8/libsoup-$libsoup_version.tar.xz
libsoup_sha256=5013dcef9136f6f861e6529fca578af50c1f52a39219027fb5aff652d1371a04

meson_version=1.12.1

if [ "$1" = clean ]; then
	rm -rf "$sources" "$build" "$prefix"
fi
mkdir -p "$downloads" "$sources" "$build" "$prefix"

homebrew=${HOMEBREW_PREFIX:-$(brew --prefix)}
# sqlite is keg-only; without its directory libsoup would use the system's while the engine uses Homebrew's.
PKG_CONFIG_PATH="$homebrew/opt/sqlite/lib/pkgconfig:$homebrew/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

# fetch URL FILE SHA256: downloads once and refuses a file whose checksum differs.
fetch() {
	[ -f "$downloads/$2" ] || curl -fL --retry 3 -o "$downloads/$2.part" "$1"
	[ -f "$downloads/$2" ] || mv "$downloads/$2.part" "$downloads/$2"
	echo "$3  $downloads/$2" | shasum -a 256 -c - >/dev/null || { echo "$2: checksum mismatch" >&2; exit 1; }
}

# unpack NAME FILE PATCH...: a fresh copy of the source with the patches applied, redone only when
# the archive or a patch changed.
unpack() {
	name=$1
	archive=$2
	shift 2
	stamp=$(for file in "$downloads/$archive" "$@"; do shasum -a 256 < "$file"; done | shasum -a 256 | cut -d' ' -f1)
	[ "$(cat "$sources/$name.stamp" 2>/dev/null)" = "$stamp" ] && return 0
	rm -rf "$sources/$name" "$sources/$name.stamp"
	mkdir -p "$sources/$name"
	tar -xf "$downloads/$archive" -C "$sources/$name" --strip-components 1
	for patch in "$@"; do
		patch -d "$sources/$name" -p1 -s < "$patch"
	done
	echo "$stamp" > "$sources/$name.stamp"
}

fetch "$boringssl_url" "boringssl-$boringssl_commit.tar.gz" "$boringssl_sha256"
fetch "$glib_networking_url" "glib-networking-$glib_networking_version.tar.xz" "$glib_networking_sha256"
fetch "$libsoup_url" "libsoup-$libsoup_version.tar.xz" "$libsoup_sha256"

unpack boringssl "boringssl-$boringssl_commit.tar.gz" "$patches/boringssl-safari-client-hello.patch"
unpack glib-networking "glib-networking-$glib_networking_version.tar.xz" "$patches/glib-networking-boringssl.patch"
unpack libsoup "libsoup-$libsoup_version.tar.xz" "$patches/libsoup-safari.patch"

# BoringSSL: static libraries in a place of their own, so that its <openssl/...> headers are seen by
# the TLS module only and never by the engine, which builds against OpenSSL.
boringssl=$build/boringssl-install
cmake -S "$sources/boringssl" -B "$build/boringssl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$boringssl" >/dev/null
ninja -C "$build/boringssl" ssl crypto
mkdir -p "$boringssl/lib"
rm -rf "$boringssl/include"
cp -R "$sources/boringssl/include" "$boringssl/include"
cp "$build/boringssl/libssl.a" "$build/boringssl/libcrypto.a" "$boringssl/lib/"

# libsoup: Homebrew's options plus Brotli. meson comes pinned from PyPI because Homebrew's moves.
if [ "$("$build/meson/bin/meson" --version 2>/dev/null)" != "$meson_version" ]; then
	rm -rf "$build/meson"
	python3 -m venv "$build/meson"
	"$build/meson/bin/pip" install --quiet "meson==$meson_version"
fi
[ -f "$build/libsoup/build.ninja" ] || "$build/meson/bin/meson" setup "$build/libsoup" "$sources/libsoup" \
	--prefix="$prefix" --libdir=lib --buildtype=release \
	-Dbrotli=enabled -Dzstd=enabled -Dgssapi=disabled -Dntlm=disabled -Dtls_check=false \
	-Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled -Dtests=false -Dautobahn=disabled \
	-Dsysprof=disabled -Dpkcs11_tests=disabled >/dev/null
"$build/meson/bin/meson" install -C "$build/libsoup" >/dev/null

# The TLS module.
cmake -S "$root/Source/GIOBoringSSL" -B "$build/gioboringssl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DGLIB_NETWORKING_SOURCE_DIR="$sources/glib-networking" -DBORINGSSL_ROOT="$boringssl" \
	-DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
ninja -C "$build/gioboringssl" install >/dev/null

mkdir -p "$prefix/bin"
cc -O2 -o "$prefix/bin/soup-get" "$here/soup-get.c" \
	$(PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$PKG_CONFIG_PATH" pkg-config --cflags --libs libsoup-3.0)

echo "built into $prefix:"
ls -l "$prefix/lib/libsoup-3.0.0.dylib" "$prefix/lib/gio/modules/libgioboringssl.so" "$prefix/bin/soup-get"
