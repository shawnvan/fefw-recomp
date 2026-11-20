# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a source-complete, game-data-free public release ZIP.

The payload hashes ($modSite / $modRuntime) are computed here from the built
artefacts and injected into Install.ps1's @@MOD_SITE@@ / @@MOD_RUNTIME@@
placeholders, so the installer always matches the payload it ships with.
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PUBLIC = ROOT / "public-release"
OUT = ROOT / "work" / "packages" / "FEFW CoreMods 0.2.8 公开版 2026-11-20.zip"
DLL = ROOT / "work" / "build-rtdll-asm" / "fefw_rt.dll"
SITES = ROOT / "recomp" / "games" / "fefw" / "main.sites"
INSTALLER = PUBLIC / "Install.ps1"
BANNED_NAMES = {"fefw.exe", "items.json", "controls.ini", "fefw.ini", "fefw.log", "pipeline_cache.bin"}
BANNED_SUFFIXES = {".nup", ".nso", ".nca", ".nsp", ".xci", ".keys", ".sav"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_commit() -> str:
    """The commit of the tree being packaged; refuses a dirty tree (the payload must be reproducible from it)."""
    status = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT).decode().strip()
    if status:
        raise RuntimeError(f"The source tree has uncommitted changes:\n{status}\nCommit them before packaging.")
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT).decode().strip()


def payload_build_id(dll: Path) -> str:
    """The build id embedded in fefw_rt.dll (runtime build_info.cpp; 'unknown' when configured outside a checkout)."""
    data = dll.read_bytes()
    marker = data.find(b"fefw-recomp-build:")
    if marker < 0:
        raise RuntimeError("fefw_rt.dll carries no build id: reconfigure CMake and rebuild (runtime/CMakeLists.txt embeds it)")
    end = marker
    while end < len(data) and 0x20 <= data[end] < 0x7f:
        end += 1
    return data[marker + len(b"fefw-recomp-build:"):end].decode("ascii")


def check(path: Path) -> bytes:
    data = path.read_bytes()
    if path.name.lower() in BANNED_NAMES or path.suffix.lower() in BANNED_SUFFIXES:
        raise RuntimeError(f"Forbidden package file: {path}")
    if data[:4] in (b"NSO0", b"NRO0", b"NCA3", b"PFS0", b"HFS0", b"BNSH", b"SARC"):
        raise RuntimeError(f"Game-derived file: {path}")
    for secret in (str(ROOT), os.environ.get("USERPROFILE", "")):
        if len(secret) < 8:
            continue
        for variant in (secret, secret.replace("\\", "/")):
            for encoding in ("utf-8", "utf-16le"):
                if variant.encode(encoding).lower() in data.lower():
                    raise RuntimeError(f"Local path embedded in {path}")
    return data


def main() -> None:
    cache = (ROOT / "work" / "build-rtdll-asm" / "CMakeCache.txt").read_text(encoding="utf-8")
    if "NXRT_DAV1D_ASM:BOOL=ON" not in cache:
        raise RuntimeError("AV1 assembly is disabled in the runtime build")
    asm_dir = ROOT / "work" / "build-rtdll-asm" / "third_party" / "dav1d" / "asm"
    if len(list(asm_dir.glob("*.obj"))) != 25:
        raise RuntimeError("AV1 assembly objects are missing")
    if not DLL.is_file() or not SITES.is_file():
        raise RuntimeError(f"Built payload missing: build fefw_rt.dll (FEFW_RT_DLL=ON) first")

    # The payload must be a build of THIS tree: the git commit embedded in fefw_rt.dll at configure time has to be the
    # commit being packaged, and the tree has to be clean. A DLL left over from an older build fails here.
    commit = source_commit()
    embedded = payload_build_id(DLL)
    if embedded != commit:
        raise RuntimeError(f"fefw_rt.dll was built from {embedded!r}, but the tree being packaged is {commit!r}. "
                           "Reconfigure CMake and rebuild the runtime, then package again.")
    print(f"payload bound to source commit {commit}")

    mod_site, mod_runtime = sha256(SITES), sha256(DLL)
    installer = INSTALLER.read_text(encoding="utf-8")
    if "@@MOD_SITE@@" not in installer or "@@MOD_RUNTIME@@" not in installer:
        raise RuntimeError("Install.ps1 no longer carries the @@MOD_SITE@@ / @@MOD_RUNTIME@@ placeholders")
    installer = installer.replace("@@MOD_SITE@@", mod_site).replace("@@MOD_RUNTIME@@", mod_runtime)
    print(f"payload main.sites SHA-256: {mod_site}")
    print(f"payload fefw_rt.dll SHA-256: {mod_runtime}")

    files = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    files = [name for name in files if name and (ROOT / name).is_file()]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    source = OUT.with_name("FEFW-CoreMods-source-0.2.8-2026-11-20.zip")
    with zipfile.ZipFile(source, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in sorted(files):
            archive.writestr(name.replace("\\", "/"), check(ROOT / name))
        dependencies = ROOT / "work" / "cmake-deps"
        for folder in sorted(dependencies.glob("*-src")):
            for path in sorted(folder.rglob("*")):
                if path.is_file() and ".git" not in path.relative_to(folder).parts:
                    archive.writestr("third_party_build_sources/" + folder.name + "/" +
                                     path.relative_to(folder).as_posix(), check(path))
        bundled = ROOT / "tools" / "textures" / "third_party"
        for path in sorted(bundled.rglob("*")):
            if path.is_file() and ".git" not in path.relative_to(bundled).parts:
                archive.writestr("tools/textures/third_party/" + path.relative_to(bundled).as_posix(), check(path))
    with zipfile.ZipFile(OUT, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in ("install.cmd", "uninstall.cmd", "generate_catalog.py", "README.txt", "使用说明.txt",
                     "来源与授权.txt", "故障处理.txt", "更新日志.txt", "问题修复记录.txt"):
            archive.writestr("CoreMods-public/" + name, check(PUBLIC / name))
        archive.writestr("CoreMods-public/Install.ps1", installer.encode("utf-8"))
        archive.writestr("CoreMods-public/payload/fefw_rt.dll", check(DLL), compress_type=zipfile.ZIP_STORED)
        archive.writestr("CoreMods-public/payload/main.sites", check(SITES))
        archive.writestr("CoreMods-public/source.zip", check(source), compress_type=zipfile.ZIP_STORED)
        archive.writestr("CoreMods-public/LICENSE.txt", check(ROOT / "LICENSE"))
    print(f"Candidate: {OUT} ({OUT.stat().st_size // 1048576} MiB)")
    print(f"Source: {source} ({source.stat().st_size // 1048576} MiB)")


if __name__ == "__main__":
    main()
