#!/bin/sh
# Builds and packages the macOS engine for the Mac it runs on: Homebrew's
# dependencies (Brewfile), the patched libsoup and the BoringSSL TLS backend
# (build-deps.sh), configure-wpe-darwin.sh, the build, and package-engine.sh
# into OUT_DIR.
# Usage: build-engine.sh TARGET OUT_DIR   (TARGET: darwin-arm64 or darwin-x64)
#
# Runners: darwin-arm64 on macos-15 (Apple silicon), darwin-x64 on
# macos-15-intel; a developer Mac of either kind works the same. Needs Xcode or
# its command line tools and Homebrew. The engine bundles Homebrew's libraries,
# so it runs on the macOS version it was built on and later.
# With CCACHE_DIR set, compiles go through ccache (WebKitCCache.cmake picks up
# the ccache the Brewfile installs), so a run cut short by a time limit
# resumes from the cache.
set -e
target=$1
out=$2
case $target in
darwin-arm64) arch=arm64 ;;
darwin-x64) arch=x86_64 ;;
*) echo "usage: $0 darwin-arm64|darwin-x64 OUT_DIR" >&2; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: $0 $target OUT_DIR" >&2; exit 2; }
[ "$(uname -m)" = "$arch" ] || { echo "build-engine: $target builds on an $arch Mac, not $(uname -m)" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

# Homebrew's pkg-config answers with paths in the command line tools' SDK
# (libffi's headers, for one); compiling against another SDK (Xcode's, where it
# is the selected developer directory) puts that SDK's C headers ahead of
# libc++'s.
if [ -z "$DEVELOPER_DIR" ] && [ -d /Library/Developer/CommandLineTools/SDKs ]; then
	export DEVELOPER_DIR=/Library/Developer/CommandLineTools
fi

brew bundle --no-upgrade --file="$here/Brewfile"
sh "$here/build-deps.sh"
sh "$root/WebKitBuild/configure-wpe-darwin.sh"
nice ninja -C "$root/WebKitBuild/wpe-darwin"
sh "$root/WebKitBuild/package-engine.sh" "$root/WebKitBuild/wpe-darwin" "$out"
