#!/bin/sh
# Builds the Windows engine for TARGET (windows-x64 or windows-arm64) from
# nothing and packages it into OUT_DIR (package-engine-win.sh): a
# cross-compilation with clang-cl and lld-link against an xwin splat of the
# Windows SDK and CRT and MSYS2's clang64/clangarm64 libraries.
#
# Runner: ubuntu-24.04 (x64; ubuntu-24.04-arm works too), for either target.
# It installs what is missing, with sudo:
#   apt: cmake ninja-build pkgconf python3 perl libjson-pp-perl ruby gperf unifdef
#        libglib2.0-dev-bin g++ zstd curl ca-certificates gnupg ccache, and clang-23 lld-23 llvm-23 from
#        apt.llvm.org (llvm-toolchain-noble-23)
#   xwin 0.10.0 (GitHub release binary), which splats the Windows SDK 10.0.26100
#   and the MSVC 14.44 CRT (accepting Microsoft's license) into win/xwin
#   MSYS2 packages and clang's Windows builtins (win/fetch-deps.py)
# On another Linux distribution the same tools have to be installed already
# (CI uses LLVM 23; 22 builds it too).
# On macOS it needs Homebrew's llvm, lld, cmake, ninja, pkgconf, gperf and glib, and
# xwin on PATH (`cargo install xwin`) unless XWIN_DIR names a splat.
#
# Environment:
#   CCACHE_DIR       compile through ccache with this cache (clang-cl's options
#                    are understood by ccache 4.9; the source path must not
#                    start with a clang-cl option's name, as /Users or /work do,
#                    or nothing is cached)
#   XWIN_DIR         an existing xwin splat to use instead of win/xwin
#   WEBKIT_LLVM_BIN  the LLVM tools' directory (default: /usr/lib/llvm-23/bin,
#                    or Homebrew's llvm)
#   JOBS, NICE       ninja's parallelism (default: the number of CPUs) and the
#                    build's niceness (default: 10)
#
# Usage: build-engine.sh windows-x64|windows-arm64 OUT_DIR
set -e
case $1 in
windows-x64) arch=x64 xwin_arch=x86_64 ;;
windows-arm64) arch=arm64 xwin_arch=aarch64 ;;
*) echo "usage: $0 windows-x64|windows-arm64 OUT_DIR" >&2; exit 2 ;;
esac
[ -n "$2" ] || { echo "usage: $0 windows-x64|windows-arm64 OUT_DIR" >&2; exit 2; }
mkdir -p "$2"
out=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
llvm_version=23
xwin_version=0.10.0
sdk_version=10.0.26100
crt_version=14.44.17.14

sudo_if_needed() {
	if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi
}

# The runners' name resolution fails now and then: package lists and downloads are retried.
apt_get() {
	sudo_if_needed apt-get -q -o Acquire::Retries=5 "$@"
}
apt_update() {
	for attempt in 1 2 3 4 5; do
		apt_get update && return 0
		sleep $((attempt * 10))
	done
	return 1
}
download() {
	curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 "$@"
}

case $(uname -s) in
Linux)
	cpus=$(nproc)
	if command -v apt-get >/dev/null; then
		packages="cmake ninja-build pkgconf python3 perl libjson-pp-perl ruby gperf unifdef libglib2.0-dev-bin g++ zstd curl ca-certificates gnupg"
		[ -n "$CCACHE_DIR" ] && packages="$packages ccache"
		missing=
		for package in $packages clang-$llvm_version lld-$llvm_version llvm-$llvm_version; do
			dpkg -s "$package" >/dev/null 2>&1 || missing="$missing $package"
		done
		if [ -n "$missing" ]; then
			export DEBIAN_FRONTEND=noninteractive
			apt_update
			apt_get install -y --no-install-recommends $packages
			if ! dpkg -s "llvm-$llvm_version" >/dev/null 2>&1; then
				codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
				download https://apt.llvm.org/llvm-snapshot.gpg.key | sudo_if_needed tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
				echo "deb http://apt.llvm.org/$codename/ llvm-toolchain-$codename-$llvm_version main" \
					| sudo_if_needed tee /etc/apt/sources.list.d/llvm-$llvm_version.list >/dev/null
				apt_update
			fi
			apt_get install -y --no-install-recommends clang-$llvm_version lld-$llvm_version llvm-$llvm_version
		fi
		: "${WEBKIT_LLVM_BIN:=/usr/lib/llvm-$llvm_version/bin}"
	else
		# Another distribution: the tools above are installed already, LLVM's on PATH.
		: "${WEBKIT_LLVM_BIN:=$(dirname "$(command -v clang-cl || echo /usr/bin/clang-cl)")}"
	fi
	;;
Darwin)
	: "${WEBKIT_LLVM_BIN:=$(brew --prefix llvm)/bin}"
	cpus=$(sysctl -n hw.ncpu)
	;;
*)
	echo "$0: cross-compiles from Linux or macOS only" >&2
	exit 1
	;;
esac
export WEBKIT_LLVM_BIN
echo "build-engine: $1 on $(uname -sm), LLVM $("$WEBKIT_LLVM_BIN/clang-cl" --version | head -1)"

# The Windows SDK and CRT.
if [ -z "$XWIN_DIR" ]; then
	XWIN_DIR=$here/xwin
	if [ ! -f "$XWIN_DIR/sdk/include/um/windows.h" ] || [ ! -d "$XWIN_DIR/crt/lib/$xwin_arch" ]; then
		xwin=$(command -v xwin || true)
		if [ -z "$xwin" ]; then
			[ "$(uname -s)" = Linux ] || { echo "$0: install xwin (cargo install xwin) or set XWIN_DIR" >&2; exit 1; }
			mkdir -p "$here/tools"
			asset=xwin-$xwin_version-$(uname -m)-unknown-linux-musl
			download "https://github.com/Jake-Shadle/xwin/releases/download/$xwin_version/$asset.tar.gz" \
				| tar -xz -C "$here/tools" --strip-components 1 "$asset/xwin"
			xwin=$here/tools/xwin
		fi
		rm -rf "$XWIN_DIR"
		"$xwin" --accept-license --cache-dir "$here/xwin-cache" --arch x86_64,aarch64 \
			--sdk-version "$sdk_version" --crt-version "$crt_version" splat --output "$XWIN_DIR"
		rm -rf "$here/xwin-cache"
	fi
fi
export XWIN_DIR

python3 "$here/fetch-deps.py" "$arch"

if [ -n "$CCACHE_DIR" ]; then
	command -v ccache >/dev/null || { echo "$0: CCACHE_DIR is set but there is no ccache" >&2; exit 1; }
	mkdir -p "$CCACHE_DIR"
	export CCACHE_DIR
	export CCACHE_BASEDIR="$root"
	: "${CCACHE_MAXSIZE:=8G}"
	export CCACHE_MAXSIZE
	# What ccache needs to reuse objects compiled against a precompiled header (toolchain.cmake).
	export CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
	launcher=ccache
	ccache -z >/dev/null
else
	launcher=
fi
sh "$root/WebKitBuild/configure-wpe-win.sh" "$arch" \
	-DCMAKE_C_COMPILER_LAUNCHER="$launcher" -DCMAKE_CXX_COMPILER_LAUNCHER="$launcher"

build=$root/WebKitBuild/wpe-win-$arch
nice -n "${NICE:-10}" ninja -C "$build" -j "${JOBS:-$cpus}"
[ -n "$launcher" ] && ccache -s

sh "$root/WebKitBuild/package-engine-win.sh" "$arch" "$build" "$out"
