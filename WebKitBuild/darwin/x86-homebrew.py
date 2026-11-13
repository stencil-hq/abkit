#!/usr/bin/env python3
"""Pours Homebrew's last x86_64 macOS bottles of FORMULA... and their runtime dependencies into PREFIX.

Homebrew no longer builds Intel macOS bottles and its installer refuses Intel Macs, but the bottles it
built until then (tag `sonoma` and older) stay on ghcr.io. For each formula this takes the newest
version that has one and pours it as `brew install` would: unpacked into PREFIX/Cellar, Homebrew's
placeholders relocated to PREFIX (text files and Mach-O load commands, re-signed ad hoc), its
.bottle/etc and .bottle/var copied into PREFIX, PREFIX/opt/NAME pointing at it and, unless the
formula is keg-only, its files linked into PREFIX. Post-install steps do not run. A formula already
poured at that version is left alone. The .pc files Homebrew keeps for macOS's own libraries (libffi,
zlib, ...) come from the Mac's Homebrew (HOMEBREW_REPOSITORY, else `brew --repository`).

build-engine.sh pours the Brewfile this way to build darwin-x64 on an Apple-silicon Mac, with PREFIX
/usr/local: the prefix the bottles were built for, which the ones Homebrew could not make relocatable
need.

Usage: x86-homebrew.py PREFIX FORMULA...

Fails when a formula (or a runtime dependency) has no x86_64 macOS bottle at all, when a download does
not match its digest, or when install_name_tool or codesign fails.
"""

import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REGISTRY = "https://ghcr.io/v2/homebrew/core"
FORMULAE = "https://formulae.brew.sh/api/formula.json"
# ghcr.io's anonymous token for public images, as brew sends it.
AUTHORIZATION = "Bearer QQ=="
INDEX = "application/vnd.oci.image.index.v1+json"
LINKED = ("bin", "sbin", "include", "lib", "share", "etc", "Frameworks")
MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xcf"}
# Versions tried per formula, newest first, before giving up on finding an x86_64 bottle.
MAX_TRIES = 60


def fetch(url, accept=None):
    headers = {"Authorization": AUTHORIZATION}
    if accept:
        headers["Accept"] = accept
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            if attempt == 3:
                raise
        except OSError:
            if attempt == 3:
                raise


def image(name):
    """The ghcr.io repository of a formula, named the way brew names it."""
    return name.replace("@", "/").replace("+", "x")


def tag_key(tag):
    """Orders bottle tags `VERSION[_REVISION][-REBUILD]` from oldest to newest."""
    match = re.fullmatch(r"(.+?)(?:_(\d+))?(?:-(\d+))?", tag)
    parts = tuple((0, int(part), "") if part.isdigit() else (1, 0, part) for part in re.findall(r"\d+|[A-Za-z]+", match[1]))
    return parts, int(match[2] or 0), int(match[3] or 0)


def newest_x86_bottle(name):
    """(tag, bottle digest, tab) of the newest version of NAME with an x86_64 macOS bottle, or None."""
    listing = fetch(f"{REGISTRY}/{image(name)}/tags/list?n=100000")
    if not listing:
        return None
    # Versions are numbers, or for some (x264) a revision such as r3222.
    tags = sorted((tag for tag in json.loads(listing)["tags"] if re.match(r"r?\d", tag)), key=tag_key, reverse=True)
    for tag in tags[:MAX_TRIES]:
        index = fetch(f"{REGISTRY}/{image(name)}/manifests/{tag}", INDEX)
        if not index:
            continue
        # Either built for x86_64 macOS or for every system (a `.all` bottle, without a platform).
        bottles = [
            manifest for manifest in json.loads(index).get("manifests", [])
            if "sh.brew.tab" in manifest.get("annotations", {}) and (
                (manifest.get("platform") or {}).get("os") == "darwin" and manifest["platform"].get("architecture") == "amd64"
                or manifest["annotations"].get("org.opencontainers.image.ref.name", "").endswith(".all"))
        ]
        if bottles:
            # The newest macOS the version was bottled for.
            bottle = max(bottles, key=lambda manifest: tag_key((manifest.get("platform") or {}).get("os.version", "0").split()[-1]))
            annotations = bottle["annotations"]
            return tag, annotations["sh.brew.bottle.digest"], json.loads(annotations["sh.brew.tab"])
    return None


def resolve(requested, aliases, catalog_names):
    """Every formula to pour, newest x86_64 bottles: the requested ones and their runtime dependencies,
    and how each requested name resolved.

    An alias names the current formula, which for a versioned one (icu4c is icu4c@79) can postdate
    Homebrew's last Intel bottles; it then stands for the version the other formulas depend on, or
    else the newest version that has one."""
    formulas = {}
    chosen = {}

    def add(names):
        pending = list(names)
        with concurrent.futures.ThreadPoolExecutor(16) as pool:
            while pending:
                names = sorted({name for name in pending if name not in formulas})
                pending = []
                for name, bottle in zip(names, pool.map(newest_x86_bottle, names)):
                    formulas[name] = None
                    if bottle:
                        tag, digest, tab = bottle
                        formulas[name] = {"tag": tag, "digest": digest, "tab": tab}
                        pending += [dependency["full_name"] for dependency in tab.get("runtime_dependencies") or []]

    add(aliases.get(name, name) for name in requested)
    for name in requested:
        target = aliases.get(name, name)
        if formulas.get(target):
            chosen[name] = target
            continue
        base = target.split("@")[0]
        siblings = [other for other, formula in formulas.items() if formula and other.split("@")[0] == base]
        if not siblings:
            versioned = sorted((other for other in catalog_names if other.startswith(base + "@")),
                               key=lambda other: tag_key(other.split("@")[1]), reverse=True)
            for candidate in versioned + [base]:
                add([candidate])
                if formulas.get(candidate):
                    siblings = [candidate]
                    break
        if not siblings:
            sys.exit(f"x86-homebrew: no version of {name} has an x86_64 macOS bottle")
        chosen[name] = max(siblings, key=lambda other: tag_key(other.split("@")[1] if "@" in other else "0"))

    missing = [name for name, formula in formulas.items() if formula is None and name in {
        dependency["full_name"] for formula in formulas.values() if formula
        for dependency in formula["tab"].get("runtime_dependencies") or []}]
    if missing:
        sys.exit(f"x86-homebrew: no x86_64 macOS bottle of {', '.join(missing)}, which others depend on")
    return {name: formula for name, formula in formulas.items() if formula}, chosen


def download(name, digest, cache):
    path = cache / f"{image(name).replace('/', '@')}--{digest}.tar.gz"
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        partial = path.with_suffix(".part")
        # curl, unlike urllib, drops the authorization on the redirect to the blob storage.
        subprocess.run(["curl", "-fsSL", "--retry", "4", "-H", f"Authorization: {AUTHORIZATION}",
                        "-o", str(partial), f"{REGISTRY}/{image(name)}/blobs/sha256:{digest}"], check=True)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != digest:
            sys.exit(f"x86-homebrew: the bottle of {name} does not match its digest {digest}")
        partial.rename(path)
    return path


def is_macho(path):
    try:
        with open(path, "rb") as file:
            header = file.read(8)
    except OSError:
        return False
    # A Java class file starts like a fat binary, then has its version (45 and up) where a fat
    # binary counts its few architectures.
    if header[:4] == b"\xca\xfe\xba\xbe":
        return int.from_bytes(header[4:8], "big") < 20
    return header[:4] in MACHO_MAGICS


def relocate_macho(path, replace):
    """Rewrites the load commands of PATH that name a Homebrew placeholder; True if it changed."""
    commands = subprocess.run(["otool", "-l", str(path)], check=True, capture_output=True, text=True).stdout
    arguments = []
    command = None
    for line in commands.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            command = line.split()[1]
        elif line.startswith(("name ", "path ")) and "@@HOMEBREW_" in line:
            old = line.split(None, 1)[1].rsplit(" (offset", 1)[0]
            new = replace(old)
            if command == "LC_ID_DYLIB":
                arguments += ["-id", new]
            elif command == "LC_RPATH":
                arguments += ["-rpath", old, new]
            else:
                arguments += ["-change", old, new]
    if not arguments:
        return False
    os.chmod(path, os.stat(path).st_mode | 0o200)
    subprocess.run(["install_name_tool", *arguments, str(path)], check=True, stderr=subprocess.DEVNULL)
    return True


def pour(name, formula, archive, prefix):
    cellar = prefix / "Cellar" / name
    # The digest and keg of the bottle poured last.
    marker = cellar / ".x86-homebrew"
    if marker.exists():
        digest, _, version = marker.read_text().partition(" ")
        if digest == formula["digest"] and (cellar / version).is_dir():
            return cellar / version
    cellar.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=prefix / "Cellar") as staging:
        subprocess.run(["tar", "-xzf", str(archive), "-C", staging], check=True)
        # A bottle holds NAME/VERSION, VERSION with the formula's revision.
        (unpacked,) = Path(staging, name).iterdir()
        keg = cellar / unpacked.name
        shutil.rmtree(keg, ignore_errors=True)
        unpacked.rename(keg)

    placeholders = {
        "@@HOMEBREW_PREFIX@@": str(prefix),
        "@@HOMEBREW_CELLAR@@": str(prefix / "Cellar"),
        "@@HOMEBREW_REPOSITORY@@": str(prefix / "Homebrew"),
        "@@HOMEBREW_LIBRARY@@": str(prefix / "Homebrew" / "Library"),
        "@@HOMEBREW_PERL@@": "/usr/bin/perl",
        "@@HOMEBREW_JAVA@@": str(prefix / "opt" / "openjdk" / "libexec" / "openjdk.jdk" / "Contents" / "Home"),
    }

    def replace(text):
        for placeholder, value in placeholders.items():
            text = text.replace(placeholder, value)
        return text

    for relative in formula["tab"].get("changed_files") or []:
        path = keg / relative
        if path.is_symlink() or not path.is_file() or is_macho(path):
            continue
        data = path.read_bytes()
        relocated = data
        for placeholder, value in placeholders.items():
            relocated = relocated.replace(placeholder.encode(), value.encode())
        if relocated != data:
            mode = path.stat().st_mode
            os.chmod(path, mode | 0o200)
            path.write_bytes(relocated)
            os.chmod(path, mode)

    for directory, _, files in os.walk(keg):
        for file in files:
            path = Path(directory, file)
            if not path.is_symlink() and is_macho(path) and relocate_macho(path, replace):
                subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True, capture_output=True)

    # What the bottle keeps for the prefix's etc and var, without replacing what is there.
    for kept in ("etc", "var"):
        source = keg / ".bottle" / kept
        if source.is_dir():
            shutil.copytree(source, prefix / kept, symlinks=True, dirs_exist_ok=True,
                            copy_function=lambda source, target: os.path.exists(target) or shutil.copy2(source, target))

    marker.write_text(f"{formula['digest']} {keg.name}")
    return keg


def link(keg, prefix):
    """Links the keg's files into PREFIX as `brew link` does; a path already there stays."""
    for top in LINKED:
        for directory, _, files in os.walk(keg / top):
            target_directory = prefix / Path(directory).relative_to(keg)
            target_directory.mkdir(parents=True, exist_ok=True)
            for file in files:
                target = target_directory / file
                if not target.exists() and not target.is_symlink():
                    target.symlink_to(Path(directory, file))


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: x86-homebrew.py PREFIX FORMULA...")
    prefix = Path(sys.argv[1]).resolve()
    requested = sys.argv[2:]

    catalog = json.loads(fetch(FORMULAE))
    aliases = {}
    keg_only = {}
    for entry in catalog:
        keg_only[entry["name"]] = bool(entry.get("keg_only"))
        for alias in (entry.get("aliases") or []) + (entry.get("oldnames") or []):
            aliases[alias] = entry["name"]

    formulas, chosen = resolve(requested, aliases, keg_only.keys())
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "abkit-x86-bottles"
    cache.mkdir(parents=True, exist_ok=True)
    (prefix / "Cellar").mkdir(parents=True, exist_ok=True)
    (prefix / "opt").mkdir(parents=True, exist_ok=True)

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        archives = dict(zip(formulas, pool.map(lambda name: download(name, formulas[name]["digest"], cache), formulas)))

    for name, formula in sorted(formulas.items()):
        keg = pour(name, formula, archives[name], prefix)
        opt = prefix / "opt" / name
        if opt.is_symlink() or opt.exists():
            opt.unlink()
        opt.symlink_to(keg)
        # Versioned formulas are keg-only even when no longer in the catalog.
        if not keg_only.get(name, "@" in name):
            link(keg, prefix)
        print(f"{name} {keg.name}")

    # pkgconf finds the .pc files of the macOS libraries (libffi, zlib, ...) in brew's repository,
    # which the Homebrew of this Mac has (HOMEBREW_REPOSITORY when brew cannot run, as under sudo).
    repository = os.environ.get("HOMEBREW_REPOSITORY") or subprocess.run(
        ["brew", "--repository"], capture_output=True, text=True).stdout.strip()
    system_pc = Path(repository, "Library", "Homebrew", "os", "mac", "pkgconfig")
    if not system_pc.is_dir():
        sys.exit("x86-homebrew: needs Homebrew installed, for the .pc files of the macOS libraries")
    shutil.copytree(system_pc, prefix / "Homebrew" / "Library" / "Homebrew" / "os" / "mac" / "pkgconfig", dirs_exist_ok=True)

    # brew --prefix NAME answers for an alias with the formula it names.
    for name, target in chosen.items():
        opt = prefix / "opt" / name
        if target != name:
            if opt.is_symlink():
                opt.unlink()
            opt.symlink_to(prefix / "opt" / target)


if __name__ == "__main__":
    main()
