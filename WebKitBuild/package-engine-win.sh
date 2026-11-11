#!/bin/sh
# Gathers a Windows WPE build into one relocatable engine directory: the
# library, its helper processes and the injected bundle side by side (where
# the library looks for them), ABHost.exe, the program that runs the engine as
# a browser for a parent process, with its web process extension where it
# looks for it (extensions\), and every DLL they import from the build and
# from the MSYS2 sysroot, plus GIO's TLS module and fontconfig's
# configuration. No Visual C++ runtime: the build links it into every module
# (win/toolchain.cmake), and the UCRT is Windows' own.
# Windows finds a DLL beside the executable that loads it, and GIO and
# fontconfig look for their modules and configuration relative to their own
# DLLs (lib\gio\modules and etc\fonts beside them), so the directory works
# wherever it is copied with nothing set in the environment.
# Usage: package-engine-win.sh x64|arm64 BUILD_DIR OUT_DIR
# llvm-objdump comes from WEBKIT_LLVM_BIN, else PATH.
set -e
arch=$1
build=$2
out=$3
case $arch in
x64) environment=clang64 ;;
arm64) environment=clangarm64 ;;
*) arch= ;;
esac
[ -n "$arch" ] && [ -d "$build/bin" ] && [ -n "$out" ] || { echo "usage: $0 x64|arm64 BUILD_DIR OUT_DIR" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
deps=$here/win/sysroot/$environment
objdump=${WEBKIT_LLVM_BIN:+$WEBKIT_LLVM_BIN/}llvm-objdump
command -v "$objdump" >/dev/null || { echo "$0: no llvm-objdump (set WEBKIT_LLVM_BIN)" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/extensions" "$out/lib/gio/modules" "$out/etc"
cp "$build/bin/WPEWebKit-2.0.dll" "$build/bin/ABWebProcess.exe" "$build/bin/ABNetworkProcess.exe" "$build/bin/ABHost.exe" "$out/"
cp "$build/bin/extensions/ABHostExtension.dll" "$out/extensions/"
# The injected bundle is a module, wherever the generator put it.
find "$build/bin" "$build/lib" -maxdepth 1 -name '*WPEInjectedBundle*.dll' -exec cp {} "$out/" \;
# The remote inspector's frontend, which the engine loads from beside the library.
if [ -f "$build/share/inspector.gresource" ]; then
	cp "$build/share/inspector.gresource" "$out/"
fi
# TLS for libsoup (OpenSSL's, over the Windows certificate store): GIO looks
# for modules under lib\gio\modules of the folder its own DLL is in.
cp "$deps/lib/gio/modules/libgioopenssl.dll" "$out/lib/gio/modules/"
# Where fontconfig looks from its own DLL: the system's fonts, per fonts.conf;
# conf.d's links are copied as the files they name.
cp -RL "$deps/etc/fonts" "$out/etc/fonts"

# Every DLL imported, transitively, that the build or the sysroot provides (the
# rest are Windows' own). The import tables name
# DLLs in any case, and Windows matches them in any case.
lower() {
	printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}
packaged() {
	[ -n "$(find "$out" -maxdepth 1 -iname "$1")" ]
}
todo=$(find "$out" -name '*.dll' -o -name '*.exe')
while [ -n "$todo" ]; do
	next=
	for file in $todo; do
		for name in $("$objdump" -p "$file" | awk '/DLL Name:/ { print $3 }'); do
			packaged "$name" && continue
			for from in "$build/bin" "$deps/bin"; do
				for candidate in "$from/$name" "$from/$(lower "$name")"; do
					if [ -e "$candidate" ]; then
						cp "$candidate" "$out/$name"
						next="$next $out/$name"
						break 2
					fi
				done
			done
		done
	done
	todo=$next
done

# What is left must be Windows' own: a DLL that is neither in the package nor
# in System32 would stop the engine from starting on a machine without it
# (vcruntime140.dll and msvcp140.dll included: the build links the C++
# runtime statically).
windows_dll() {
	case $(lower "$1") in
	api-ms-win-*|ext-ms-*|kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|\
	ws2_32.dll|crypt32.dll|bcrypt.dll|ncrypt.dll|secur32.dll|shlwapi.dll|dnsapi.dll|iphlpapi.dll|winmm.dll|\
	dwmapi.dll|uxtheme.dll|usp10.dll|version.dll|userenv.dll|netapi32.dll|rpcrt4.dll|comctl32.dll|comdlg32.dll|\
	dbghelp.dll|psapi.dll|ntdll.dll|ucrtbase.dll|setupapi.dll|imm32.dll|msimg32.dll|winhttp.dll|wininet.dll|\
	d3d9.dll|d3d11.dll|d3d12.dll|dxgi.dll|d3dcompiler_47.dll|opengl32.dll|mfplat.dll|mf.dll|mfreadwrite.dll|\
	propsys.dll|shcore.dll|powrprof.dll|wtsapi32.dll|normaliz.dll|mswsock.dll|cfgmgr32.dll|hid.dll|winspool.drv|\
	dwrite.dll|windowscodecs.dll|wldap32.dll|credui.dll|msvcrt.dll|synchronization.dll) return 0 ;;
	*) return 1 ;;
	esac
}
missing=$(find "$out" -name '*.dll' -o -name '*.exe' | while read -r file; do
	"$objdump" -p "$file" | awk '/DLL Name:/ { print $3 }'
done | sort -fu | while read -r name; do
	packaged "$name" || windows_dll "$name" || echo "$name"
done)
if [ -n "$missing" ]; then
	echo "package-engine-win: imported but neither packaged nor a Windows DLL:" $missing >&2
	exit 1
fi
du -sh "$out"
ls "$out" | wc -l
