#!/bin/sh
# Installs a packaged engine (the directory package-engine.sh writes) where omp
# and Tern look for it: ~/.o2/lib/abkit, whose ABHost is the host executable.
# Usage: install-engine.sh [ENGINE_DIR]   (default: WebKitBuild/engine)
#
# The engine is more than one file, and its library finds the rest beside
# itself, so the whole directory is installed. The new copy replaces the old
# one by rename, so a host that is running keeps the files it has open.
# The engine's earlier install (~/.o2/bin/wpe linking into ~/.o2/lib/wpe) is
# removed.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
engine=${1:-$root/WebKitBuild/engine}
[ -x "$engine/ABHost" ] || [ -x "$engine/ABHost.exe" ] || { echo "no ABHost in $engine: run package-engine.sh first" >&2; exit 1; }
lib=$HOME/.o2/lib
mkdir -p "$lib"
rm -rf "$lib/abkit.new" "$lib/abkit.old"
cp -R "$engine" "$lib/abkit.new"
[ ! -d "$lib/abkit" ] || mv "$lib/abkit" "$lib/abkit.old"
mv "$lib/abkit.new" "$lib/abkit"
rm -rf "$lib/abkit.old"
rm -rf "$HOME/.o2/bin/wpe" "$lib/wpe"
echo "installed $lib/abkit"
