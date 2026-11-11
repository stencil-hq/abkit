#!/usr/bin/env python3
"""Fetches the libraries the Windows build links: MSYS2's clang64 (x64) or
clangarm64 (arm64) packages (GLib, libsoup, HarfBuzz, ICU, ...) with their
dependencies, unpacked into sysroot/<environment> beside this script, and
clang's builtins into WebKitLibraries/windows, where it also lays out the
Windows SDK and CRT of an xwin splat (XWIN_DIR, else cargo-xwin's cache) as
Source/cmake/OptionsMSVC.cmake looks for them.

All of them are C libraries on the UCRT, so the clang-cl (MSVC ABI) build links
their import libraries as they are.

Usage: fetch-deps.py x64|arm64
"""
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

# Per architecture: the MSYS2 environment, its package prefix, and the names
# xwin and clang's builtins give the architecture.
ARCHES = {
    "x64": {"environment": "clang64", "prefix": "mingw-w64-clang-x86_64-", "xwin": "x86_64", "builtins": "x86_64"},
    "arm64": {"environment": "clangarm64", "prefix": "mingw-w64-clang-aarch64-", "xwin": "aarch64", "builtins": "aarch64"},
}
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
WINDOWS_LIBRARIES = os.path.normpath(os.path.join(HERE, "..", "..", "WebKitLibraries", "windows"))
# Headers generated for MinGW's C runtime, where Microsoft's differs.
MSVC_FIXES = {
    "include/zconf.h": ("#define HAVE_UNISTD_H 1\n", "#ifndef _MSC_VER\n#define HAVE_UNISTD_H 1\n#endif\n"),
}


def download(url, path):
    """urlretrieve, retried: the mirror or name resolution fails now and then."""
    for attempt in range(5):
        try:
            urllib.request.urlretrieve(url, path)
            return
        except OSError as error:
            if attempt == 4:
                raise
            print(f"{url}: {error}; retrying", file=sys.stderr)
            time.sleep(10 * (attempt + 1))


def mirror(environment):
    return f"https://mirror.msys2.org/mingw/{environment}/"


def database(environment):
    """The repository's packages by name, and the names they provide."""
    path = os.path.join(HERE, "msys", environment + ".db")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    download(mirror(environment) + environment + ".db", path)
    out = os.path.join(HERE, "msys", "db-" + environment)
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
            "environment": environment,
            "deps": [re.split(r"[<>=]", dep)[0] for dep in fields.get("DEPENDS", []) if dep],
        }
        for provided in fields.get("PROVIDES", []):
            if provided:
                provides.setdefault(re.split(r"[<>=]", provided)[0], name)
    return packages, provides


def closure(packages, provides, prefix, roots):
    """`roots` and everything they depend on."""
    want, todo = {}, [prefix + root for root in roots]
    while todo:
        name = todo.pop()
        name = name if name in packages else provides.get(name)
        if not name or name in want or SKIP.search(name[len(prefix):]):
            continue
        want[name] = packages[name]
        todo += packages[name]["deps"]
    return want


def link_sdk(arch):
    """WebKitLibraries/windows/{sdk,crt} as links into an xwin splat, whose
    library directories are named x86_64/aarch64, or x64/arm64 when it was
    splatted with --preserve-ms-arch-notation."""
    splat = os.environ.get("XWIN_DIR")
    if not splat:
        cache = "~/Library/Caches" if sys.platform == "darwin" else "~/.cache"
        splat = os.path.expanduser(cache + "/cargo-xwin/xwin")
    if not os.path.exists(os.path.join(splat, "sdk", "include", "um", "windows.h")):
        raise SystemExit(f"no Windows SDK at {splat}: run `xwin splat` and set XWIN_DIR")
    links = [("sdk/include", "sdk/include"), ("crt/include", "crt/include")]
    for library in ("crt/lib", "sdk/lib/ucrt", "sdk/lib/um"):
        target = next(
            (f"{library}/{name}" for name in (ARCHES[arch]["xwin"], arch) if os.path.isdir(os.path.join(splat, library, name))),
            None,
        )
        if not target:
            raise SystemExit(f"no {arch} libraries in {splat}/{library}: splat with --arch {ARCHES[arch]['xwin']}")
        links.append((f"{library}/{arch}", target))
    for link, target in links:
        path = os.path.join(WINDOWS_LIBRARIES, link)
        target = os.path.join(splat, target)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.islink(path) and os.readlink(path) != target:
            os.remove(path)
        if not os.path.lexists(path):
            os.symlink(target, path)
    # The build links the C++ runtime statically and the UCRT dynamically (toolchain.cmake).
    for library in ("crt/lib/{}/libcmt.lib", "crt/lib/{}/libcpmt.lib", "crt/lib/{}/libvcruntime.lib", "sdk/lib/ucrt/{}/ucrt.lib"):
        if not os.path.exists(os.path.join(WINDOWS_LIBRARIES, library.format(arch))):
            raise SystemExit(f"the splat in {splat} has no {library.format(arch)}")


def fetch(info):
    path = os.path.join(HERE, "msys", "pkg", info["file"])
    if not os.path.exists(path):
        download(mirror(info["environment"]) + info["file"], path + ".part")
        os.rename(path + ".part", path)
    return path


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ARCHES:
        raise SystemExit(__doc__)
    arch = sys.argv[1]
    environment, prefix = ARCHES[arch]["environment"], ARCHES[arch]["prefix"]
    link_sdk(arch)
    packages, provides = database(environment)
    want = closure(packages, provides, prefix, ROOTS)
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
    libraries = os.path.join(sysroot, environment, "lib")
    for name in os.listdir(libraries):
        path = os.path.join(libraries, name)
        if name.endswith(".dll.a"):
            for alias in (name[len("lib"):-len(".dll.a")] + ".lib", name[:-len(".dll.a")] + ".lib"):
                shutil.copyfile(path, os.path.join(libraries, alias))
        elif name.endswith(".a"):
            os.remove(path)
    for header, (old, new) in MSVC_FIXES.items():
        path = os.path.join(sysroot, environment, header)
        with open(path) as file:
            text = file.read()
        if old not in text:
            raise SystemExit(f"{header}: nothing to adapt, the package changed")
        with open(path, "w") as file:
            file.write(text.replace(old, new, 1))
    print(len(want), "packages in", sysroot)

    # clang's builtins (128-bit arithmetic and the like) for the target.
    runtime = fetch(packages[prefix + "compiler-rt"])
    scratch = os.path.join(HERE, "compiler-rt")
    os.makedirs(scratch, exist_ok=True)
    subprocess.run(["tar", "--zstd", "-xf", runtime, "-C", scratch, "--exclude", ".*"], check=True)
    builtins = f"clang_rt.builtins-{ARCHES[arch]['builtins']}"
    for folder, _, files in os.walk(os.path.join(scratch, environment)):
        if f"lib{builtins}.a" in files:
            target = os.path.join(WINDOWS_LIBRARIES, builtins + ".lib")
            shutil.copyfile(os.path.join(folder, f"lib{builtins}.a"), target)
            print("builtins:", target)
            break
    else:
        raise SystemExit(f"no lib{builtins}.a in {os.path.basename(runtime)}")


if __name__ == "__main__":
    main()
