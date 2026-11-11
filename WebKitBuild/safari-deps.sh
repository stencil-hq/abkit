# Sourced, not run. What every engine build shares of the libraries that make the engine look like
# Safari to a server: the pinned sources (version and checksum), and the steps that build them.
# - libsoup with Brotli, the HTTP/2 preface, the ALPN list and the request header order Safari
#   sends (darwin/patches/libsoup-safari.patch);
# - libgioboringssl.so, the GIO TLS backend on BoringSSL (Source/GIOBoringSSL) that sends Safari's
#   ClientHello: glib-networking's OpenSSL backend (darwin/patches/glib-networking-boringssl.patch)
#   linked statically against BoringSSL (darwin/patches/boringssl-safari-client-hello.patch).
#
# The caller sets, before sourcing:
#   root       the WebKit checkout
#   downloads  where archives are kept between runs
#   sources    where patched sources are unpacked
#   build      where build directories go
#   prefix     where libsoup and the TLS module are installed
#   meson      the meson executable to use (see safari_deps_meson)
# and may set SAFARI_DEPS_CMAKE_ARGS (extra arguments for the CMake builds, such as a compiler
# launcher).
#
# Used by linux/build-deps.sh. darwin/build-deps.sh predates this file and carries the same pins;
# a change to one belongs in both until it sources this file too.

safari_deps_patches=$root/WebKitBuild/darwin/patches

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

safari_deps_sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@" | cut -d' ' -f1
	else
		shasum -a 256 "$@" | cut -d' ' -f1
	fi
}

# fetch URL FILE SHA256: downloads once and refuses a file whose checksum differs.
fetch() {
	[ -f "$downloads/$2" ] || curl -fsSL --retry 5 --retry-all-errors -o "$downloads/$2.part" "$1"
	[ -f "$downloads/$2" ] || mv "$downloads/$2.part" "$downloads/$2"
	[ "$(safari_deps_sha256 "$downloads/$2")" = "$3" ] || { echo "$2: checksum mismatch" >&2; exit 1; }
}

# unpack NAME FILE PATCH...: a fresh copy of the source with the patches applied, redone only when
# the archive or a patch changed.
unpack() {
	name=$1
	archive=$2
	shift 2
	stamp=$(for file in "$downloads/$archive" "$@"; do safari_deps_sha256 "$file"; done | safari_deps_sha256 -)
	[ "$(cat "$sources/$name.stamp" 2>/dev/null)" = "$stamp" ] && return 0
	rm -rf "$sources/$name" "$sources/$name.stamp"
	mkdir -p "$sources/$name"
	tar -xf "$downloads/$archive" -C "$sources/$name" --strip-components 1
	for patch in "$@"; do
		patch -d "$sources/$name" -p1 -s < "$patch"
	done
	echo "$stamp" > "$sources/$name.stamp"
}

# safari_deps_meson [PYTHON_PACKAGE...]: meson pinned from PyPI in a virtual environment of its own
# (distributions' move, or are too old), with the Python packages other builds want beside it.
safari_deps_meson() {
	if [ "$("$build/meson/bin/meson" --version 2>/dev/null)" != "$meson_version" ]; then
		rm -rf "$build/meson"
		python3 -m venv "$build/meson"
		"$build/meson/bin/pip" install --quiet "meson==$meson_version"
	fi
	[ $# = 0 ] || "$build/meson/bin/pip" install --quiet "$@"
	meson=$build/meson/bin/meson
}

# safari_deps_fetch [GLIB_NETWORKING_PATCH...]: the three sources, patched; extra patches apply to
# glib-networking after the shared one.
safari_deps_fetch() {
	fetch "$boringssl_url" "boringssl-$boringssl_commit.tar.gz" "$boringssl_sha256"
	fetch "$glib_networking_url" "glib-networking-$glib_networking_version.tar.xz" "$glib_networking_sha256"
	fetch "$libsoup_url" "libsoup-$libsoup_version.tar.xz" "$libsoup_sha256"
	unpack boringssl "boringssl-$boringssl_commit.tar.gz" "$safari_deps_patches/boringssl-safari-client-hello.patch"
	unpack glib-networking "glib-networking-$glib_networking_version.tar.xz" "$safari_deps_patches/glib-networking-boringssl.patch" "$@"
	unpack libsoup "libsoup-$libsoup_version.tar.xz" "$safari_deps_patches/libsoup-safari.patch"
}

# BoringSSL: static libraries in a place of their own, so that its <openssl/...> headers are seen by
# the TLS module only and never by the engine, which builds against OpenSSL.
safari_deps_build_boringssl() {
	boringssl=$build/boringssl-install
	cmake -S "$sources/boringssl" -B "$build/boringssl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$boringssl" \
		$SAFARI_DEPS_CMAKE_ARGS >/dev/null
	ninja -C "$build/boringssl" ssl crypto
	mkdir -p "$boringssl/lib"
	rm -rf "$boringssl/include"
	cp -R "$sources/boringssl/include" "$boringssl/include"
	cp "$build/boringssl/libssl.a" "$build/boringssl/libcrypto.a" "$boringssl/lib/"
}

# libsoup: the options distributions build it with, plus Brotli. Extra meson arguments are passed on.
safari_deps_build_libsoup() {
	[ -f "$build/libsoup/build.ninja" ] || rm -rf "$build/libsoup"
	[ -f "$build/libsoup/build.ninja" ] || "$meson" setup "$build/libsoup" "$sources/libsoup" \
		--prefix="$prefix" --libdir=lib --buildtype=release \
		-Dbrotli=enabled -Dzstd=enabled -Dgssapi=disabled -Dntlm=disabled -Dtls_check=false \
		-Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled -Dtests=false -Dautobahn=disabled \
		-Dsysprof=disabled -Dpkcs11_tests=disabled "$@"
	"$meson" install -C "$build/libsoup" --quiet
}

# The TLS module, installed in $prefix/lib/gio/modules.
safari_deps_build_gioboringssl() {
	cmake -S "$root/Source/GIOBoringSSL" -B "$build/gioboringssl" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DGLIB_NETWORKING_SOURCE_DIR="$sources/glib-networking" -DBORINGSSL_ROOT="$boringssl" \
		-DCMAKE_INSTALL_PREFIX="$prefix" $SAFARI_DEPS_CMAKE_ARGS >/dev/null
	ninja -C "$build/gioboringssl" install >/dev/null
}
