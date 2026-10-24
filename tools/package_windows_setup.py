#!/usr/bin/env python3
"""Build and verify the single-file Windows offline GUI installer."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
import zipfile

from package_release import (FIDELITYFX_LICENSE, FIDELITYFX_LICENSE_PATH,
                             FIDELITYFX_LICENSE_SHA256, MAX_ARCHIVE, MAX_FILES,
                             MAX_MEMBER, MAX_TOTAL, SEMVER, Payload, allowed_path,
                             architecture, digest, fail, package_windows,
                             pe_imports, verify_archive, windows_system)

SEVENZIP_INSTALLER_SHA256 = "d54bf805f9f3704d1e8db2fa3498ae7ef2df0312b40b558e7c71c734430a665d"
SEVENZIP_URL = "https://github.com/ip7z/7zip/releases/download/26.04/7z2604-x64.exe"
LZMA_SDK_URL = "https://github.com/ip7z/7zip/releases/download/26.04/lzma2604.7z"
LZMA_SDK_SHA256 = "bd98058d0e12c5da970dd4e5e1d8fa24f2d704914d96e69c28f92ed45421a439"
SFX_SHA256 = "837a2d84a4d15366af7a91670908aa29a958b721f40db46da734621f6234fea2"
SFX_SIZE = 129024
SEVENZIP_HASHES = {
    "7z.exe": "95fff19b73eff77c87c0d7bd32822e43fb90dd2932459f962eb532d8dfb6ba53",
    "7z.dll": "899cd73d7fc60e1c1c35c7de0f5bed3dc69bfe2cecbd48014d868a347970a0b1",
    "License.txt": "519ac0a4bded9c18ea02e0afb71f663d8c47373bd9facd3ac96a79f51d77765d",
    "readme.txt": "6929cb624c82d5d1c55a146b5dcb4542445628e4ad8bcfe6272a0814e1383618",
    "History.txt": "cf51c5e78c0cf4b9106ad6b42918a471d15c78a993a9df5b5efccb1377823063",
}
CONFIG = (b';!@Install@!UTF-8!\nTitle="ETI Yami Setup"\n'
          b'RunProgram="runtime\\yami-setup.exe"\n;!@InstallEnd@!\n')
LICENSES = ("License.txt", "readme.txt", "History.txt")


def run(cwd: Path, *args: str | Path) -> None:
    environment = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "GH_TOKEN", "GITHUB_TOKEN"):
        environment.pop(name, None)
    subprocess.run([str(arg) for arg in args], cwd=cwd, env=environment,
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def extract_official(installer: Path, output: Path) -> None:
    if digest(installer) != SEVENZIP_INSTALLER_SHA256:
        fail(f"Expected pinned official 7-Zip installer: {SEVENZIP_URL}")
    run(output.parent, "7z", "e", installer, f"-o{output}", *SEVENZIP_HASHES, "-y")
    for name, expected in SEVENZIP_HASHES.items():
        if digest(output / name) != expected:
            fail(f"Unverified official 7-Zip member: {name}")

def extract_bootstrap(sdk: Path, output: Path) -> None:
    if digest(sdk) != LZMA_SDK_SHA256:
        fail(f"Expected pinned official LZMA SDK: {LZMA_SDK_URL}")
    run(output.parent, "7z", "e", sdk, "bin/7zSD.sfx", f"-o{output.parent}", "-y")
    if digest(output) != SFX_SHA256:
        fail("Unverified official automatic-extraction SFX module")



def check_closure(root: Path, arch: str) -> None:
    files = [path for path in root.iterdir() if path.is_file()]
    names = {path.name.casefold() for path in files}
    for path in files:
        if path.suffix.lower() not in (".exe", ".dll"):
            continue
        architecture(path, "windows", arch)
        for imported in pe_imports(path):
            key = imported.casefold()
            if key not in names and not windows_system(key):
                fail(f"Unbundled Windows dependency: {path.name} -> {imported}")


def check_remover(path: Path, arch: str) -> None:
    architecture(path, "windows", arch)
    unexpected = sorted(name for name in pe_imports(path) if not windows_system(name.casefold()))
    if unexpected:
        fail("Removal helper must be statically linked against its C/C++ runtime: " + ", ".join(unexpected))


def verify_payload(root: Path, arch: str, engine_archive: Path | None) -> None:
    required = {f"runtime/{name}" for name in ("yami-setup.exe", "yami-remove.exe", "7zz.exe", "7z.dll")}
    required |= {FIDELITYFX_LICENSE_PATH} | {f"licenses/7zip/{name}" for name in LICENSES}
    actual: set[str] = set()
    total = 0
    for path in root.rglob("*"):
        if path.is_symlink() or (not path.is_file() and not path.is_dir()):
            fail(f"Unsafe Windows installer member: {path}")
        if path.is_dir():
            continue
        name = path.relative_to(root).as_posix()
        parts = PurePosixPath(name).parts
        valid = ((len(parts) == 2 and parts[0] == "engine" and allowed_path(parts[1], "windows"))
                 or (len(parts) == 2 and parts[0] == "runtime"
                     and parts[1].lower().endswith((".exe", ".dll")))
                 or name in required)
        if not valid or name.casefold() in {item.casefold() for item in actual}:
            fail(f"Unexpected or colliding Windows installer member: {name}")
        size = path.stat().st_size
        if size == 0 or size > MAX_MEMBER or total + size > MAX_TOTAL or len(actual) >= MAX_FILES:
            fail(f"Windows installer payload bounds exceeded: {name}")
        actual.add(name)
        total += size
    if not required.issubset(actual):
        fail("Windows installer omits setup/remover/archiver/license files: " + str(required - actual))
    check_closure(root / "runtime", arch)
    check_closure(root / "engine", arch)
    check_remover(root / "runtime/yami-remove.exe", arch)
    if digest(root / FIDELITYFX_LICENSE_PATH) != FIDELITYFX_LICENSE_SHA256:
        fail("Unverified FidelityFX license")
    for name in LICENSES:
        if digest(root / "licenses/7zip" / name) != SEVENZIP_HASHES[name]:
            fail(f"Unverified 7-Zip license file: {name}")
    if (digest(root / "runtime/7zz.exe") != SEVENZIP_HASHES["7z.exe"]
            or digest(root / "runtime/7z.dll") != SEVENZIP_HASHES["7z.dll"]):
        fail("Installer archiver differs from the pinned official 7-Zip binaries")
    if engine_archive is not None:
        verify_archive(engine_archive, "windows", arch)
        with zipfile.ZipFile(engine_archive) as engine:
            expected = {member.filename for member in engine.infolist()}
            actual_engine = {path.relative_to(root / "engine").as_posix()
                             for path in (root / "engine").rglob("*") if path.is_file()}
            if expected != actual_engine:
                fail(f"Installer engine differs from update ZIP: {expected ^ actual_engine}")
            for member in engine.infolist():
                if digest(root / "engine" / member.filename) != hashlib.sha256(engine.read(member)).hexdigest():
                    fail(f"Installer engine bytes differ from update ZIP: {member.filename}")


def verify_sfx(archive: Path, arch: str, engine_archive: Path | None = None) -> None:
    archive = archive.absolute()
    if archive.name != f"yami-setup-windows-{arch}.exe" or archive.stat().st_size > MAX_ARCHIVE + SFX_SIZE + len(CONFIG):
        fail("Expected bounded Windows installer release asset")
    data = archive.read_bytes()
    if (len(data) <= SFX_SIZE + len(CONFIG)
            or hashlib.sha256(data[:SFX_SIZE]).hexdigest() != SFX_SHA256
            or data[SFX_SIZE:SFX_SIZE + len(CONFIG)] != CONFIG):
        fail("Windows installer SFX/config policy mismatch")
    run(archive.parent, "7z", "t", archive)
    with tempfile.TemporaryDirectory(prefix="yami-windows-setup-verify-") as temporary:
        root = Path(temporary) / "payload"
        run(Path(temporary), "7z", "x", archive, f"-o{root}", "-y")
        verify_payload(root, arch, engine_archive)


def build(args: argparse.Namespace) -> None:
    verify_archive(args.engine_archive, "windows", args.arch)
    if args.output.exists():
        fail(f"Refusing to replace existing installer: {args.output}")
    with tempfile.TemporaryDirectory(prefix="yami-windows-setup-") as temporary:
        work = Path(temporary)
        official = work / "official"
        official.mkdir()
        extract_official(args.sevenzip_installer.resolve(strict=True), official)
        bootstrap = work / "7zSD.sfx"
        extract_bootstrap(args.lzma_sdk.resolve(strict=True), bootstrap)
        payload = Payload(work / "payload", "windows", args.arch,
                          extra_paths=("runtime/yami-setup.exe", "runtime/yami-remove.exe",
                                       "runtime/7zz.exe", "runtime/7z.dll"))
        setup = args.setup_binary.resolve(strict=True)
        remover = args.remover_binary.resolve(strict=True)
        check_remover(remover, args.arch)
        payload.copy(official / "7z.exe", "runtime/7zz.exe")
        payload.copy(official / "7z.dll", "runtime/7z.dll")
        package_windows(payload, [official / "7z.exe"], [official])
        payload.copy(setup, "runtime/yami-setup.exe")
        payload.copy(remover, "runtime/yami-remove.exe")
        package_windows(payload, [setup, remover],
                        [path.resolve(strict=True) for path in args.search_dir])
        for path in list(payload.root.iterdir()):
            if path.is_file() and path.suffix.lower() == ".dll":
                path.rename(payload.root / "runtime" / path.name)
        (payload.root / "engine").mkdir()
        with zipfile.ZipFile(args.engine_archive) as engine:
            for member in engine.infolist():
                with engine.open(member) as source, (payload.root / "engine" / member.filename).open("xb") as target:
                    shutil.copyfileobj(source, target)
        license_path = payload.root / FIDELITYFX_LICENSE_PATH
        license_path.parent.mkdir(parents=True)
        shutil.copyfile(FIDELITYFX_LICENSE, license_path)
        seven_licenses = payload.root / "licenses/7zip"
        seven_licenses.mkdir(parents=True)
        for name in LICENSES:
            shutil.copyfile(official / name, seven_licenses / name)
        verify_payload(payload.root, args.arch, args.engine_archive)
        packed = work / "installer.7z"
        run(payload.root, "7z", "a", "-t7z", "-mx=9", "-ms=on", packed,
            "runtime", "engine", "licenses")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("xb") as output:
            output.write(bootstrap.read_bytes())
            output.write(CONFIG)
            output.write(packed.read_bytes())
    verify_sfx(args.output, args.arch, args.engine_archive)
    print(f"Packaged {args.output.name}: sha256:{digest(args.output)}")


def regression_checks(archive: Path, arch: str, engine_archive: Path | None) -> None:
    verify_sfx(archive, arch, engine_archive)
    original = archive.read_bytes()
    with tempfile.TemporaryDirectory(prefix="yami-windows-setup-regression-") as temporary:
        root = Path(temporary)
        for name, data in (("truncated", original[:-97]),
                           ("config", original[:SFX_SIZE] + b"X" + original[SFX_SIZE + 1:]),
                           ("payload", original[:-64] + bytes(original[-64] ^ 1) + original[-63:])):
            mutated = root / name / archive.name
            mutated.parent.mkdir()
            mutated.write_bytes(data)
            try:
                verify_sfx(mutated, arch)
            except (RuntimeError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile):
                pass
            else:
                raise AssertionError(f"Verifier accepted {name} Windows installer")
    print("Passed Windows installer integrity, payload, PE/DLL closure, license and corruption regressions")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup-binary", type=Path)
    parser.add_argument("--remover-binary", type=Path)
    parser.add_argument("--engine-archive", type=Path)
    parser.add_argument("--sevenzip-installer", type=Path)
    parser.add_argument("--lzma-sdk", type=Path)
    parser.add_argument("--search-dir", type=Path, action="append", default=[])
    parser.add_argument("--arch", choices=("x86_64",), required=True)
    parser.add_argument("--version")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.output.name != f"yami-setup-windows-{args.arch}.exe":
        parser.error("Expected architecture-specific Windows installer filename")
    if args.self_test:
        regression_checks(args.output, args.arch, args.engine_archive)
    elif args.verify_only:
        verify_sfx(args.output, args.arch, args.engine_archive)
        print(f"Verified {args.output.name}: sha256:{digest(args.output)}")
    else:
        if (not args.setup_binary or not args.remover_binary or not args.engine_archive
                or not args.sevenzip_installer or not args.lzma_sdk or not args.search_dir
                or not args.version or not SEMVER.fullmatch(args.version)):
            parser.error("Build requires setup/remover, engine ZIP, official 7-Zip, DLL search path and stable version")
        build(args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"Windows installer packaging failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors="replace"), file=sys.stderr)
        sys.exit(1)
