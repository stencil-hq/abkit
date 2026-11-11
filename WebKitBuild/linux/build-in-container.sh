#!/bin/sh
# What build-engine.sh runs inside the build container: dependencies, configure, build, package.
# Usage: build-in-container.sh TARGET OUT_DIR
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
target=$1
out=$2
work=$root/WebKitBuild/linux/work/$target
jobs=${JOBS:-$(nproc)}

echo "== $target: dependencies"
sh "$here/build-deps.sh" "$work"

# The build runs tools of the prefix (GLib's glib-compile-resources), which need its libraries
# rather than the distribution's older ones.
LD_LIBRARY_PATH=$work/prefix/lib
export LD_LIBRARY_PATH

echo "== $target: configure"
sh "$root/WebKitBuild/configure-wpe-linux.sh" "$work/webkit" "$work/prefix"

echo "== $target: build"
ccache --zero-stats >/dev/null 2>&1 || true
nice -n "${NICE:-0}" ninja -C "$work/webkit" -j "$jobs" ${NINJA_ARGS:-}
ccache --show-stats 2>/dev/null || true

echo "== $target: package"
sh "$root/WebKitBuild/package-engine-linux.sh" "$work/webkit" "$work/engine" "$work/prefix"
# OUT_DIR is a mount point, so its contents are replaced rather than the directory.
find "$out" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
cp -a "$work/engine/." "$out/"
echo "== $target: packaged into $out"
