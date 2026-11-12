#!/bin/sh
# Builds, inside the build container (build-engine.sh), what the Linux engine needs and the
# container's distribution does not provide as needed, into WORK_DIR/prefix:
# - GLib 2.84, Brotli 1.1 and fontconfig 2.15 (glibc only: Ubuntu 22.04 has GLib 2.72, and
#   glib-networking needs 2.73.3; it has Brotli 1.0.9, and the patched libsoup needs 1.1; its
#   fontconfig 2.13 ignores the relative font directory the package names its fonts by);
# - libsoup and the BoringSSL TLS module that send what Safari sends (../safari-deps.sh), the
#   module with patches/glib-networking-boringssl-linux.patch on top of the Darwin patch: Safari's
#   ClientHello and session reuse on Linux too (the engine's user agent names Safari on every
#   platform), and the system's CA bundle found wherever a distribution keeps it;
# - Mesa's EGL and GLES on llvmpipe alone, with LLVM linked in statically: the engine draws with
#   EGL, and a package that brings its own renders on a machine without a GPU or any GL installed.
# Sources are pinned by version and checksum; downloads are kept in linux/downloads.
#
# Usage: build-deps.sh WORK_DIR
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$1
[ -n "$work" ] || { echo "usage: $0 WORK_DIR" >&2; exit 2; }
mkdir -p "$work"
work=$(cd "$work" && pwd)
downloads=$here/downloads
sources=$work/src
build=$work/build
prefix=$work/prefix
mkdir -p "$downloads" "$sources" "$build" "$prefix"

. "$root/WebKitBuild/safari-deps.sh"

glib_version=2.84.4
glib_url=https://download.gnome.org/sources/glib/2.84/glib-$glib_version.tar.xz
glib_sha256=8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90

brotli_version=1.1.0
brotli_url=https://github.com/google/brotli/archive/refs/tags/v$brotli_version.tar.gz
brotli_sha256=e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff

fontconfig_version=2.15.0
fontconfig_url=https://www.freedesktop.org/software/fontconfig/release/fontconfig-$fontconfig_version.tar.xz
fontconfig_sha256=63a0658d0e06e0fa886106452b58ef04f21f58202ea02a94c39de0d3335d7c0e

mesa_version=25.1.9
mesa_url=https://archive.mesa3d.org/mesa-$mesa_version.tar.xz
mesa_sha256=412df33a1bb3c785ed698555a3972118a37c458e7accf6ae53f4bb87b3db454a

PKG_CONFIG_PATH=$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
export PKG_CONFIG_PATH
# Everything compiles through ccache when there is one, as the engine does.
if command -v ccache >/dev/null 2>&1; then
	SAFARI_DEPS_CMAKE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
	meson_cc="ccache $CC"
	meson_cxx="ccache $CXX"
else
	meson_cc=$CC
	meson_cxx=$CXX
fi

# meson with what Mesa's generators import, in the Python that meson's builds find first.
safari_deps_meson mako packaging pyyaml
PATH=$build/meson/bin:$PATH
export PATH

# meson_build NAME [OPTION...]: configures once, then builds and installs into the prefix.
meson_build() {
	name=$1
	shift
	[ -f "$build/$name/build.ninja" ] || rm -rf "$build/$name"
	[ -f "$build/$name/build.ninja" ] || CC=$meson_cc CXX=$meson_cxx "$meson" setup "$build/$name" "$sources/$name" \
		--prefix="$prefix" --libdir=lib --buildtype=release --wrap-mode=nofallback "$@"
	"$meson" install -C "$build/$name" --quiet
}

if [ "$(ldd --version 2>&1 | head -n 1 | grep -ci 'glibc\|gnu libc')" != 0 ]; then
	fetch "$glib_url" "glib-$glib_version.tar.xz" "$glib_sha256"
	unpack glib "glib-$glib_version.tar.xz"
	meson_build glib -Dtests=false -Dintrospection=disabled -Dman-pages=disabled -Ddocumentation=false \
		-Dnls=disabled -Dselinux=disabled -Dlibmount=disabled -Dsysprof=disabled -Dglib_debug=disabled

	fetch "$brotli_url" "brotli-$brotli_version.tar.gz" "$brotli_sha256"
	unpack brotli "brotli-$brotli_version.tar.gz"
	cmake -S "$sources/brotli" -B "$build/brotli" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBROTLI_DISABLE_TESTS=ON \
		$SAFARI_DEPS_CMAKE_ARGS >/dev/null
	ninja -C "$build/brotli" install >/dev/null

	# The package always names its configuration (package-engine-linux.sh), so the font
	# directories built in only matter to a build-tree run, where they are the system's.
	fetch "$fontconfig_url" "fontconfig-$fontconfig_version.tar.xz" "$fontconfig_sha256"
	unpack fontconfig "fontconfig-$fontconfig_version.tar.xz"
	meson_build fontconfig -Ddoc=disabled -Dnls=disabled -Dtests=disabled -Dtools=disabled \
		-Dcache-build=disabled -Ddefault-fonts-dirs=/usr/share/fonts \
		-Dadditional-fonts-dirs=/usr/local/share/fonts -Dcache-dir=/var/cache/fontconfig
fi

safari_deps_fetch "$here/patches/glib-networking-boringssl-linux.patch"
safari_deps_build_boringssl
(CC=$meson_cc CXX=$meson_cxx; export CC CXX; safari_deps_build_libsoup)
safari_deps_build_gioboringssl

fetch "$mesa_url" "mesa-$mesa_version.tar.xz" "$mesa_sha256"
unpack mesa "mesa-$mesa_version.tar.xz" "$here/patches/mesa-warnings.patch"
llvm_config=$(command -v "${LLVM_CONFIG:-llvm-config}")
printf '[binaries]\nllvm-config = '"'%s'"'\n' "$llvm_config" > "$build/mesa-native.ini"
meson_build mesa --native-file "$build/mesa-native.ini" \
	-Dplatforms= -Dgallium-drivers=llvmpipe -Dvulkan-drivers= -Degl=enabled -Dgles1=disabled -Dgles2=enabled \
	-Dopengl=true -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled -Dllvm=enabled -Dshared-llvm=disabled \
	-Ddraw-use-llvm=true -Dxmlconfig=disabled -Dexpat=disabled -Dzstd=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dtools= -Dvideo-codecs= \
	-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-rusticl=false -Dteflon=false \
	-Dmicrosoft-clc=disabled -Dspirv-to-dxil=false

echo "built into $prefix:"
ls -l "$prefix/lib/libsoup-3.0.so.0" "$prefix/lib/gio/modules/libgioboringssl.so" "$prefix/lib/libEGL.so.1"
