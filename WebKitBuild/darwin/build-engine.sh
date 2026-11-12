#!/bin/sh
# Builds and packages the macOS engine for the Mac it runs on: Homebrew's
# dependencies (Brewfile), the patched libsoup and the BoringSSL TLS backend
# (build-deps.sh), configure-wpe-darwin.sh, the build, and package-engine.sh
# into OUT_DIR.
# Usage: build-engine.sh TARGET OUT_DIR   (TARGET: darwin-arm64 or darwin-x64)
#
# Runners: Blacksmith's macos-26 (Apple silicon) for both. Homebrew no longer
# builds for Intel Macs, so darwin-x64 on Apple silicon takes the Brewfile's
# last x86_64 bottles, poured into /usr/local (x86-homebrew.py, with sudo),
# and runs the rest of this script under Rosetta, where every tool sees an
# x86_64 Mac; the compiler still runs natively, Apple silicon's clang with
# -arch x86_64 (wrappers in darwin/x86-clang). An Intel Mac with Homebrew
# builds darwin-x64 as an Apple-silicon one builds darwin-arm64.
# Needs Xcode or its command line tools and Homebrew. The engine bundles
# Homebrew's libraries, so it runs on the macOS version it was built on and
# later.
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
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
if [ "$target" = darwin-x64 ] && [ "$(uname -m)" = arm64 ]; then
	/usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null || sudo softwareupdate --install-rosetta --agree-to-license
	sudo -H /usr/bin/env HOMEBREW_REPOSITORY="$(brew --repository)" \
		python3 "$here/x86-homebrew.py" /usr/local $(sed -n 's/^brew "\([^"]*\)".*/\1/p' "$here/Brewfile")
	mkdir -p "$here/x86-clang"
	for compiler in clang clang++; do
		printf '#!/bin/sh\nexec /usr/bin/arch -arm64 /usr/bin/%s -arch x86_64 "$@"\n' "$compiler" > "$here/x86-clang/$compiler"
		chmod +x "$here/x86-clang/$compiler"
	done
	exec /usr/bin/arch -x86_64 /usr/bin/env PATH=/usr/local/bin:/usr/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin \
		HOMEBREW_PREFIX=/usr/local CC="$here/x86-clang/clang" CXX="$here/x86-clang/clang++" /bin/sh "$0" "$@"
fi
[ "$(uname -m)" = "$arch" ] || { echo "build-engine: $target builds on an $arch Mac, not $(uname -m)" >&2; exit 2; }

# Homebrew's pkg-config answers with paths in the command line tools' SDK
# (libffi's headers, for one); compiling against another SDK (Xcode's, where it
# is the selected developer directory) puts that SDK's C headers ahead of
# libc++'s.
if [ -z "$DEVELOPER_DIR" ] && [ -d /Library/Developer/CommandLineTools/SDKs ]; then
	export DEVELOPER_DIR=/Library/Developer/CommandLineTools
fi

# Under Rosetta the Brewfile is already poured.
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" != 1 ]; then
	brew bundle --no-upgrade --file="$here/Brewfile"
fi
sh "$here/build-deps.sh"
sh "$root/WebKitBuild/configure-wpe-darwin.sh"
nice ninja -C "$root/WebKitBuild/wpe-darwin"
sh "$root/WebKitBuild/package-engine.sh" "$root/WebKitBuild/wpe-darwin" "$out"
