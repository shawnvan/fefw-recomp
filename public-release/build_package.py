# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a source-complete, game-data-free public release ZIP.

The payload hashes ($modSite / $modRuntime) are computed here from the built
artefacts and injected into Install.ps1's @@MOD_SITE@@ / @@MOD_RUNTIME@@
placeholders, so the installer always matches the payload it ships with.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "work" / "packages" / "FEFW CoreMods 0.2.8 公开版 2026-11-20.zip"
BANNED_NAMES = {"fefw.exe", "items.json", "controls.ini", "fefw.ini", "fefw.log", "pipeline_cache.bin"}
BANNED_SUFFIXES = {".nup", ".nso", ".nca", ".nsp", ".xci", ".keys", ".sav"}


def source_commit(checkout: Path) -> str:
    status = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=checkout, text=True
    ).strip()
    if status:
        raise RuntimeError(f"Tracked source has uncommitted changes in {checkout}:\n{status}\nCommit them before packaging.")
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=checkout, text=True).strip()


def payload_build_id(data: bytes) -> str:
    """The build id embedded in fefw_rt.dll (runtime build_info.cpp; 'unknown' when configured outside a checkout)."""
    marker = data.find(b"fefw-recomp-build:")
    if marker < 0:
        raise RuntimeError("fefw_rt.dll carries no build id: reconfigure CMake and rebuild (runtime/CMakeLists.txt embeds it)")
    end = marker
    while end < len(data) and 0x20 <= data[end] < 0x7f:
        end += 1
    return data[marker + len(b"fefw-recomp-build:"):end].decode("ascii")


def check(path: Path, checkout: Path) -> bytes:
    data = path.read_bytes()
    if path.name.lower() in BANNED_NAMES or path.suffix.lower() in BANNED_SUFFIXES:
        raise RuntimeError(f"Forbidden package file: {path}")
    if data[:4] in (b"NSO0", b"NRO0", b"NCA3", b"PFS0", b"HFS0", b"BNSH", b"SARC"):
        raise RuntimeError(f"Game-derived file: {path}")
    for secret in (str(ROOT), str(checkout), os.environ.get("USERPROFILE", "")):
        if len(secret) < 8:
            continue
        for variant in (secret, secret.replace("\\", "/")):
            for encoding in ("utf-8", "utf-16le"):
                if variant.encode(encoding).lower() in data.lower():
                    raise RuntimeError(f"Local path embedded in {path}")
    return data


def build_payload(checkout: Path, build_dir: Path, powershell: str) -> None:
    command = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
    subprocess.run(command + [str(checkout / "tools/textures/scripts/fetch_third_party.ps1")],
                   cwd=checkout, check=True)
    subprocess.run(command + [str(checkout / "game-build/scripts/build_fefw.ps1"),
                              "-Target", "fefw_rt", "-BuildDir", str(build_dir),
                              "-CMakeArgs", "-DFEFW_RT_DLL=ON -DNXRT_DAV1D_ASM=ON"], cwd=checkout, check=True)
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    if "NXRT_DAV1D_ASM:BOOL=ON" not in cache.splitlines():
        raise RuntimeError("AV1 assembly is disabled in the isolated build")
    asm_dir = build_dir / "third_party" / "dav1d" / "asm"
    if len(list(asm_dir.glob("*.obj"))) != 25:
        raise RuntimeError("AV1 assembly objects are missing")


def package_payload(checkout: Path, build_dir: Path, commit: str, out: Path, source: Path) -> None:
    if source_commit(checkout) != commit:
        raise RuntimeError("The isolated checkout changed during the build")
    public = checkout / "public-release"
    sites = check(checkout / "recomp/games/fefw/main.sites", checkout)
    dll = check(build_dir / "fefw_rt.dll", checkout)
    embedded = payload_build_id(dll)
    if embedded != commit:
        raise RuntimeError(f"fefw_rt.dll reports build {embedded!r} in a checkout pinned to {commit!r}")
    mod_site = hashlib.sha256(sites).hexdigest()
    mod_runtime = hashlib.sha256(dll).hexdigest()
    installer = check(public / "Install.ps1", checkout).decode("utf-8")
    if "@@MOD_SITE@@" not in installer or "@@MOD_RUNTIME@@" not in installer:
        raise RuntimeError("Install.ps1 no longer carries the @@MOD_SITE@@ / @@MOD_RUNTIME@@ placeholders")
    installer = installer.replace("@@MOD_SITE@@", mod_site).replace("@@MOD_RUNTIME@@", mod_runtime)
    print(f"payload main.sites SHA-256: {mod_site}")
    print(f"payload fefw_rt.dll SHA-256: {mod_runtime}")

    files = subprocess.check_output(["git", "ls-files", "-z"], cwd=checkout).decode("utf-8").split("\0")
    with zipfile.ZipFile(source, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in sorted(filter(None, files)):
            archive.writestr(name, check(checkout / name, checkout))
        dependencies = build_dir / "_deps"
        for folder in sorted(dependencies.glob("*-src")):
            for path in sorted(folder.rglob("*")):
                if path.is_file() and ".git" not in path.relative_to(folder).parts:
                    archive.writestr("third_party_build_sources/" + folder.name + "/" +
                                     path.relative_to(folder).as_posix(), check(path, checkout))
        bundled = checkout / "tools/textures/third_party"
        for name in ("astc-encoder", "bc7enc_rdo"):
            for path in sorted((bundled / name).rglob("*")):
                if path.is_file() and ".git" not in path.relative_to(bundled).parts:
                    archive.writestr(path.relative_to(checkout).as_posix(), check(path, checkout))
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in ("install.cmd", "uninstall.cmd", "generate_catalog.py", "README.txt", "使用说明.txt",
                     "来源与授权.txt", "故障处理.txt", "更新日志.txt", "问题修复记录.txt"):
            archive.writestr("CoreMods-public/" + name, check(public / name, checkout))
        archive.writestr("CoreMods-public/Install.ps1", installer.encode("utf-8"))
        archive.writestr("CoreMods-public/payload/fefw_rt.dll", dll, compress_type=zipfile.ZIP_STORED)
        archive.writestr("CoreMods-public/payload/main.sites", sites)
        archive.writestr("CoreMods-public/source.zip", check(source, checkout), compress_type=zipfile.ZIP_STORED)
        archive.writestr("CoreMods-public/LICENSE.txt", check(checkout / "LICENSE", checkout))


def main() -> None:
    commit = source_commit(ROOT)
    powershell = shutil.which("pwsh") or shutil.which("powershell")
    if os.name != "nt" or not powershell:
        raise RuntimeError("Release packaging requires Windows, PowerShell, and the Windows build toolchain (docs/BUILDING.md)")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    source = OUT.with_name("FEFW-CoreMods-source-0.2.8-2026-11-20.zip")
    with tempfile.TemporaryDirectory(prefix="coremods-", dir=OUT.parent) as temp:
        staging = Path(temp)
        checkout = staging / "src"
        # The clone has its own Git metadata and no local changes from the developer's checkout.
        subprocess.run(["git", "clone", "--quiet", "--no-checkout", "--no-hardlinks", str(ROOT), str(checkout)], check=True)
        subprocess.run(["git", "checkout", "--quiet", "--detach", commit], cwd=checkout, check=True)
        build_dir = checkout / "work/build-rtdll"
        build_payload(checkout, build_dir, powershell)
        package_payload(checkout, build_dir, commit, staging / OUT.name, staging / source.name)
        os.replace(staging / source.name, source)
        os.replace(staging / OUT.name, OUT)
    print(f"Candidate: {OUT} ({OUT.stat().st_size // 1048576} MiB)")
    print(f"Source: {source} ({source.stat().st_size // 1048576} MiB)")


if __name__ == "__main__":
    main()
