#!/usr/bin/env python3
"""Fetches the libraries the Windows build links: MSYS2's clang64 packages
(GLib, libsoup, HarfBuzz, ICU, ...) with their dependencies, unpacked into
sysroot/ beside this script, and clang's builtins into WebKitLibraries/windows,
where it also lays out the Windows SDK and CRT of an xwin splat (XWIN_DIR,
else cargo-xwin's cache) as Source/cmake/OptionsMSVC.cmake looks for them.

All of them are C libraries on the UCRT, so the clang-cl (MSVC ABI) build links
their import libraries as they are.

Usage: fetch-deps.py
"""
import concurrent.futures
import os
import re
import shutil
import subprocess
import urllib.request

MIRROR = "https://mirror.msys2.org/mingw/clang64/"
PREFIX = "mingw-w64-clang-x86_64-"
ROOTS = [
    "glib2", "glib-networking", "libsoup3", "harfbuzz", "harfbuzz-icu", "freetype", "fontconfig",
    "icu", "libxml2", "libxslt", "sqlite3", "zlib", "libjpeg-turbo", "libpng", "libwebp",
    "openssl", "libgcrypt", "libtasn1", "woff2", "libc++", "libunwind",
]
# Build-time and optional dependencies no library needs at run time.
SKIP = re.compile(
    r"python|tcl|^tk$|gcc|^clang$|llvm|gobject-introspection|vala|cmake|ninja|meson|ncurses|readline"
    r"|gtk|qt|wineditline|ca-certificates|p11-kit|docbook|shared-mime|cairo|-docs$|-utils$"
)
HERE = os.path.dirname(os.path.abspath(__file__))
# Headers generated for MinGW's C runtime, where Microsoft's differs.
MSVC_FIXES = {
    "include/zconf.h": ("#define HAVE_UNISTD_H 1\n", "#ifndef _MSC_VER\n#define HAVE_UNISTD_H 1\n#endif\n"),
}


def database():
    """The repository's packages by name, and the names they provide."""
    path = os.path.join(HERE, "msys", "clang64.db")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    urllib.request.urlretrieve(MIRROR + "clang64.db", path)
    out = os.path.join(HERE, "msys", "db")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.run(["tar", "-xf", path, "-C", out], check=True)
    packages, provides = {}, {}
    for entry in os.listdir(out):
        with open(os.path.join(out, entry, "desc")) as desc:
            fields = {
                m.group(1): m.group(2).strip().split("\n")
                for m in re.finditer(r"%(\w+)%\n(.*?)(?:\n\n|\Z)", desc.read(), re.S)
            }
        name = fields["NAME"][0]
        packages[name] = {
            "file": fields["FILENAME"][0],
            "deps": [re.split(r"[<>=]", dep)[0] for dep in fields.get("DEPENDS", []) if dep],
        }
        for provided in fields.get("PROVIDES", []):
            if provided:
                provides.setdefault(re.split(r"[<>=]", provided)[0], name)
    return packages, provides


def closure(packages, provides, roots):
    """`roots` and everything they depend on."""
    want, todo = {}, [PREFIX + root for root in roots]
    while todo:
        name = todo.pop()
        name = name if name in packages else provides.get(name)
        if not name or name in want or SKIP.search(name[len(PREFIX):]):
            continue
        want[name] = packages[name]
        todo += packages[name]["deps"]
    return want


def link_sdk():
    """WebKitLibraries/windows/{sdk,crt} as links into an xwin splat."""
    splat = os.environ.get("XWIN_DIR") or os.path.expanduser("~/Library/Caches/cargo-xwin/xwin")
    if not os.path.exists(os.path.join(splat, "sdk", "include", "um", "windows.h")):
        raise SystemExit(f"no Windows SDK at {splat}: run `xwin splat` and set XWIN_DIR")
    root = os.path.join(HERE, "..", "..", "WebKitLibraries", "windows")
    for link, target in [
        ("sdk/include", "sdk/include"),
        ("crt/include", "crt/include"),
        ("crt/lib/x64", "crt/lib/x86_64"),
        ("sdk/lib/ucrt/x64", "sdk/lib/ucrt/x86_64"),
        ("sdk/lib/um/x64", "sdk/lib/um/x86_64"),
    ]:
        path = os.path.join(root, link)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if not os.path.lexists(path):
            os.symlink(os.path.join(splat, target), path)


def fetch(info):
    path = os.path.join(HERE, "msys", "pkg", info["file"])
    if not os.path.exists(path):
        urllib.request.urlretrieve(MIRROR + info["file"], path + ".part")
        os.rename(path + ".part", path)
    return path


def main():
    link_sdk()
    packages, provides = database()
    want = closure(packages, provides, ROOTS)
    os.makedirs(os.path.join(HERE, "msys", "pkg"), exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        archives = list(pool.map(fetch, want.values()))
    sysroot = os.path.join(HERE, "sysroot")
    os.makedirs(sysroot, exist_ok=True)
    for archive in archives:
        subprocess.run(["tar", "--zstd", "-xf", archive, "-C", sysroot, "--exclude", ".*"], check=True)
    # MinGW names an import library libNAME.dll.a; an MSVC toolchain looks
    # for NAME.lib (or libNAME.lib). The static archives beside them are for
    # MinGW alone (its C runtime and stack protector).
    libraries = os.path.join(sysroot, "clang64", "lib")
    for name in os.listdir(libraries):
        path = os.path.join(libraries, name)
        if name.endswith(".dll.a"):
            for alias in (name[len("lib"):-len(".dll.a")] + ".lib", name[:-len(".dll.a")] + ".lib"):
                shutil.copyfile(path, os.path.join(libraries, alias))
        elif name.endswith(".a"):
            os.remove(path)
    for header, (old, new) in MSVC_FIXES.items():
        path = os.path.join(sysroot, "clang64", header)
        with open(path) as file:
            text = file.read()
        if old not in text:
            raise SystemExit(f"{header}: nothing to adapt, the package changed")
        with open(path, "w") as file:
            file.write(text.replace(old, new, 1))
    print(len(want), "packages in", sysroot)

    # clang's builtins (128-bit arithmetic and the like) for the target.
    runtime = fetch(packages[PREFIX + "compiler-rt"])
    scratch = os.path.join(HERE, "compiler-rt")
    os.makedirs(scratch, exist_ok=True)
    subprocess.run(["tar", "--zstd", "-xf", runtime, "-C", scratch, "--exclude", ".*"], check=True)
    for folder, _, files in os.walk(scratch):
        if "libclang_rt.builtins-x86_64.a" in files:
            target = os.path.join(HERE, "..", "..", "WebKitLibraries", "windows", "clang_rt.builtins-x86_64.lib")
            shutil.copyfile(os.path.join(folder, "libclang_rt.builtins-x86_64.a"), target)
            print("builtins:", os.path.normpath(target))


if __name__ == "__main__":
    main()
