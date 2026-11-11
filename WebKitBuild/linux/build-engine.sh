#!/bin/sh
# Builds and packages a Linux engine in a container, leaving the packaged engine directory (what
# package-engine-linux.sh writes; smoke-engine.py checks it) at OUT_DIR, owned by the caller.
#
# Usage: build-engine.sh TARGET OUT_DIR
#   TARGET   linux-x64 | linux-arm64 | linux-musl-x64 | linux-musl-arm64
#
# Runner: GitHub's ubuntu-24.04 for the x64 targets, ubuntu-24.04-arm for the arm64 ones (the
# container runs natively; any Linux or macOS host with Docker for the same architecture does too,
# and another architecture works through emulation, slowly). The host needs docker and a POSIX
# shell, which both runners have; nothing else is installed on it.
#
# The container (built here from Dockerfile.glibc or Dockerfile.musl, beside this script):
# - glibc targets: ubuntu:22.04, so the engine needs glibc 2.35 or newer (Ubuntu 22.04+,
#   Debian 12+, Fedora 36+) and the system's libstdc++ (GCC 12 or newer); everything else it loads
#   is in the package. Packages: apt.llvm.org's clang-20 lld-20 llvm-20 llvm-20-dev libpolly-20-dev
#   libclang-rt-20-dev, and gcc g++ libstdc++-12-dev make cmake ninja-build ccache pkg-config
#   patchelf file patch xz-utils bzip2 git python3 python3-venv perl ruby gperf unifdef bison flex nasm
#   libicu-dev libharfbuzz-dev libfreetype-dev libfontconfig-dev libjpeg-dev libpng-dev
#   libwebp-dev libxml2-dev libxslt1-dev libsqlite3-dev zlib1g-dev libepoxy-dev libxkbcommon-dev
#   libwoff-dev libavif-dev liblcms2-dev libssl-dev libnghttp2-dev libpsl-dev libbrotli-dev
#   libzstd-dev libffi-dev libpcre2-dev libmount-dev libexpat1-dev libdrm-dev xkb-data
#   libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev
#   gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
#   gstreamer1.0-libav libevent-dev libasound2-dev libopus-dev libvpx-dev libmanette-0.2-dev
#   flite1-dev fonts-dejavu-core.
# - musl targets: alpine:3.22; the engine needs musl 1.2.5 or newer and nothing else of the system.
#   Packages: clang20 lld20 llvm20-dev llvm20-static llvm20-gtest compiler-rt build-base
#   linux-headers cmake samurai ccache pkgconf patchelf file patch xz bzip2 git curl bash python3
#   py3-pip perl ruby ruby-dev gperf unifdef bison flex nasm icu-dev icu-data-full harfbuzz-dev
#   freetype-dev fontconfig-dev libjpeg-turbo-dev libpng-dev libwebp-dev libxml2-dev libxslt-dev sqlite-dev
#   zlib-dev libepoxy-dev libxkbcommon-dev woff2-dev libavif-dev libjxl-dev lcms2-dev openssl-dev
#   nghttp2-dev libpsl-dev brotli-dev zstd-dev glib-dev libffi-dev pcre2-dev expat-dev libdrm-dev
#   xkeyboard-config gstreamer-dev gst-plugins-base-dev gst-plugins-bad-dev gst-plugins-base
#   gst-plugins-good gst-plugins-bad gst-libav libevent-dev alsa-lib-dev opus-dev libvpx-dev
#   libmanette-dev flite-dev font-dejavu.
# In the container linux/build-deps.sh builds GLib (glibc), libsoup, the BoringSSL TLS module and
# Mesa (pinned sources, fetched from download.gnome.org, github.com and archive.mesa3d.org),
# configure-wpe-linux.sh configures, ninja builds and package-engine-linux.sh packages.
#
# Environment:
#   CCACHE_DIR    the ccache directory, mounted into the container; every compiler run goes through
#                 it, so a run that ran out of time finishes from it. Without it the cache is kept
#                 in WebKitBuild/linux/work/TARGET/ccache.
#   JOBS          parallel jobs (default: the CPUs the container sees)
#   NICE          niceness of the build (default 0)
#   NINJA_ARGS    more arguments for the engine's ninja (such as -k 0)
#   DOCKER_BUILD_ARGS  more arguments for building the image (such as --add-host for a mirror)
# Intermediate files stay in WebKitBuild/linux/work/TARGET (about 15 GB); delete it when done.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
target=$1
out=$2
case $target in
linux-x64) flavor=glibc platform=linux/amd64 ;;
linux-arm64) flavor=glibc platform=linux/arm64 ;;
linux-musl-x64) flavor=musl platform=linux/amd64 ;;
linux-musl-arm64) flavor=musl platform=linux/arm64 ;;
*) echo "usage: $0 linux-x64|linux-arm64|linux-musl-x64|linux-musl-arm64 OUT_DIR" >&2; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: $0 TARGET OUT_DIR" >&2; exit 2; }

work=$root/WebKitBuild/linux/work/$target
ccache=${CCACHE_DIR:-$work/ccache}
mkdir -p "$work" "$ccache" "$out"
out=$(cd "$out" && pwd)
ccache=$(cd "$ccache" && pwd)

image=abkit-build:$flavor-${platform#linux/}
# The image has no context but its Dockerfile. Builds and runs use the host's network, so they
# resolve names as the host does whatever Docker's own networking makes of it.
docker build --network host --platform "$platform" ${DOCKER_BUILD_ARGS:-} -t "$image" - < "$here/Dockerfile.$flavor"

# The checkout is mounted at the same place on every run and every machine, so the compiler sees
# the same paths and ccache hits whoever runs it. The build runs as the caller, so what it writes,
# OUT_DIR included, is the caller's. The compiler is compared by content because the image, and
# its files' times, is rebuilt on every run.
exec docker run --rm --network host --platform "$platform" --user "$(id -u):$(id -g)" \
	-v "$root:/src" -v "$ccache:/ccache" -v "$out:/out" \
	-e HOME=/tmp -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}" \
	-e CCACHE_COMPILERCHECK=content -e CCACHE_BASEDIR=/src -e CCACHE_NOHASHDIR=1 \
	-e CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime \
	-e JOBS="${JOBS:-}" -e NICE="${NICE:-0}" -e NINJA_ARGS="${NINJA_ARGS:-}" \
	"$image" sh /src/WebKitBuild/linux/build-in-container.sh "$target" /out
