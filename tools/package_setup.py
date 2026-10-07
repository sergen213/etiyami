#!/usr/bin/env python3
"""Build/verify the Linux GUI shell self-extractor; Python is build-time only."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from package_release import (BINARIES, LINUX_SYSTEM, MAX_ARCHIVE, MAX_FILES, MAX_MEMBER,
                             MAX_TOTAL, SEMVER, Payload, allowed_path, architecture,
                             command, digest, fail, hash_stream, package_linux, verify_archive)

SEVENZIP_VERSION = "26.04"
SEVENZIP_BASE = "https://github.com/ip7z/7zip/releases/download/26.04/"
# Official GitHub release asset API digests, independently checked against downloads.
SEVENZIP = {
    "x86_64": ("7z2604-linux-x64.tar.xz",
               "fc0327ba27e89bd086cf426dff17d77de582953cdbbc10a6576540a06853ffcd",
               "a6fe7bcdb28e296c53e2c597817b9c6dba00953bf2c68e029489435399585729"),
    "arm64": ("7z2604-linux-arm64.tar.xz",
              "5b0ac3aa91c1e3499f011d6af69d7f6b4de3ef2f011d9e2ff44c58ebd04aa388",
              "8f34c72fa4a9a79282b2364308f75d77a928ddf571292a7d030825e53d60e3a7"),
}
SOURCE_NAME = "7z2604-src.tar.xz"
SOURCE_SHA256 = "9691944c0fe0d01bb49373a704fb983fd33bc98b1738695179dfbf99ac1734f6"
LICENSES = {
    "License.txt": "1790374e5352329cedb46ee3808930a88e9ca2f08b82b10fcf5cf605d2c301b1",
    "readme.txt": "3b0c58eac0cd3fd6b99edfbb34132ce61da992e99c1f6ee5e2543e7bc11b7fc9",
    "History.txt": "cf51c5e78c0cf4b9106ad6b42918a471d15c78a993a9df5b5efccb1377823063",
}
PROVENANCE_PATH = "licenses/7zip/provenance.json"
MARKER = re.compile(rb"# YAMI_SETUP_PAYLOAD ([0-9a-f]{64}) ([0-9]+) (x86_64|arm64) ([0-9.]+)\n")
RUNTIME_PROGRAMS = ("yami-setup", "yami-remove", "uninstall.sh", "7zz")
REMOVER_LOADERS = {"x86_64": "ld-linux-x86-64.so.2", "arm64": "ld-linux-aarch64.so.1"}
REMOVER_SYSTEM = re.compile(
    r"(?:lib(?:c|m|dl|pthread|rt|stdc\+\+|gcc_s)\.so(?:\.[0-9]+)*)\Z")
UNINSTALL_SCRIPT = Path(__file__).resolve().parent.parent / "native" / "uninstall.sh"


def provenance(arch: str) -> bytes:
    archive, archive_hash, binary_hash = SEVENZIP[arch]
    return (json.dumps({"version": SEVENZIP_VERSION, "binary": "7zzs (fully static), renamed 7zz",
                        "binary_sha256": binary_hash, "archive_url": SEVENZIP_BASE + archive,
                        "archive_sha256": archive_hash, "source_url": SEVENZIP_BASE + SOURCE_NAME,
                        "source_sha256": SOURCE_SHA256,
                        "release_api": "https://api.github.com/repos/ip7z/7zip/releases/tags/26.04"},
                       indent=2, sort_keys=True) + "\n").encode()


def shell_header(sha256: str, size: int, arch: str, version: str) -> bytes:
    # Fixed-width offset means header length is independent of its final value.
    template = '''#!/bin/sh
# YAMI_SETUP_PAYLOAD {sha256} {size} {arch} {version}
set -eu
umask 077
payload_offset={offset:020d}
error() {{ printf '%s\\n' "ETI Yami installer: $*" >&2; exit 1; }}
[ "$(uname -s)" = Linux ] || error 'Linux is required.'
case "$(uname -m)" in
    {machine}) ;;
    *) error 'This installer requires {arch}.' ;;
esac
for utility in mktemp tail wc sha256sum tar rm; do
    command -v "$utility" >/dev/null 2>&1 || error "Missing standard system utility: $utility"
done
self=$0
case "$self" in
    */*) ;;
    *) self=$(command -v "$self") || error 'Cannot locate installer.' ;;
esac
case "$self" in /*) ;; *) self=$PWD/$self ;; esac
work=$(mktemp -d "${{TMPDIR:-/tmp}}/yami-setup.XXXXXXXXXX") || error 'Cannot create private temporary directory.'
child=
cleanup() {{ rm -rf -- "$work"; }}
stop() {{
    if [ -n "$child" ]; then kill "$child" 2>/dev/null || :; wait "$child" 2>/dev/null || :; fi
    exit 1
}}
trap cleanup 0
trap stop HUP INT TERM
tail -c +"$payload_offset" -- "$self" > "$work/payload.tar.gz" || error 'Cannot read payload.'
[ "$(wc -c < "$work/payload.tar.gz")" -eq {size} ] || error 'Truncated or oversized payload.'
actual=$(sha256sum "$work/payload.tar.gz") || error 'Cannot hash payload.'
[ "${{actual%% *}}" = '{sha256}' ] || error 'Corrupt payload (SHA256 mismatch).'
# Only the exact build-time-validated tar is ever unpacked; no ISO is read here.
tar -xzf "$work/payload.tar.gz" -C "$work" --no-same-owner --no-same-permissions || error 'Cannot unpack payload.'
"$work/runtime/yami-setup" "$@" &
child=$!
status=0
wait "$child" || status=$?
child=
exit "$status"
'''
    fields = dict(sha256=sha256, size=size, arch=arch, version=version,
                  machine="x86_64" if arch == "x86_64" else "aarch64|arm64")
    initial = template.format(offset=0, **fields).encode()
    return template.format(offset=len(initial) + 1, **fields).encode()


def make_sfx(payload: Path, output: Path, arch: str, version: str) -> None:
    if payload.stat().st_size > MAX_ARCHIVE:
        fail("Compressed installer payload exceeds bounds")
    with output.open("xb") as target, payload.open("rb") as source:
        target.write(shell_header(digest(payload), payload.stat().st_size, arch, version))
        shutil.copyfileobj(source, target)
    output.chmod(0o755)


def permitted(name: str) -> bool:
    parts = name.split("/")
    if len(parts) == 2 and parts[0] in ("engine", "runtime"):
        leaf = parts[1]
        if parts[0] == "engine":
            return allowed_path(leaf, "linux") and not LINUX_SYSTEM.fullmatch(leaf)
        return (leaf in RUNTIME_PROGRAMS or
                (leaf.startswith("lib") and allowed_path(leaf, "linux") and not LINUX_SYSTEM.fullmatch(leaf)))
    return name == PROVENANCE_PATH or name in {f"licenses/7zip/{name}" for name in LICENSES}


def check_elf(path: Path, arch: str, siblings: set[str], glibc_max: str | None) -> set[str]:
    architecture(path, "linux", arch)
    if path.name == "7zz":
        if digest(path) != SEVENZIP[arch][2] or "INTERP" in command("readelf", "--program-headers", path):
            fail("7zz must be the pinned official fully static 7zzs binary")
        return set()
    if command("patchelf", "--print-rpath", path).strip() != "$ORIGIN":
        fail(f"Non-flat runtime search path: {path.name}")
    needed = set(command("patchelf", "--print-needed", path).splitlines())
    if path.name == "yami-remove":
        loader = REMOVER_LOADERS[arch]
        unexpected = sorted(name for name in needed if name != loader and not REMOVER_SYSTEM.fullmatch(name))
        if unexpected:
            fail(f"Removal helper must depend only on the host C/C++ runtime; unexpected: {', '.join(unexpected)}")
    for library in needed:
        if "/" in library or (not LINUX_SYSTEM.fullmatch(library) and library not in siblings):
            fail(f"Unbundled dependency: {path.name} -> {library}")
    if glibc_max:
        maximum = tuple(map(int, glibc_max.split(".")))
        versions = re.findall(r"\bGLIBC_([0-9]+\.[0-9]+)(?:\.[0-9]+)?\b",
                              command("readelf", "--version-info", path))
        if any(tuple(map(int, version.split("."))) > maximum for version in versions):
            fail(f"{path.name} needs glibc newer than {glibc_max}")
    return {name for name in needed if not LINUX_SYSTEM.fullmatch(name)}


def verify_sfx(archive: Path, arch: str, glibc_max: str | None = None, engine_archive: Path | None = None) -> None:
    if archive.name != f"yami-setup-linux-{arch}.run" or archive.stat().st_size > MAX_ARCHIVE + 16384:
        fail("Expected bounded installer release asset")
    expected_engine = None
    if engine_archive is not None:
        verify_archive(engine_archive, "linux", arch)
        with zipfile.ZipFile(engine_archive) as engine:
            expected_engine = {}
            for member in engine.infolist():
                with engine.open(member) as source:
                    expected_engine[member.filename] = hash_stream(source)
    with archive.open("rb") as stream:
        prefix = stream.read(16384)
        match = MARKER.match(prefix, len(b"#!/bin/sh\n"))
        if not match:
            fail("Missing installer metadata")
        sha256, size, encoded_arch, version = [part.decode() for part in match.groups()]
        if encoded_arch != arch or not SEMVER.fullmatch(version) or not 0 < int(size) <= MAX_ARCHIVE:
            fail("Invalid installer metadata")
        header = shell_header(sha256, int(size), arch, version)
        if prefix[:len(header)] != header or archive.stat().st_size != len(header) + int(size):
            fail("Installer shell policy/size mismatch")
        stream.seek(len(header))
        if hash_stream(stream) != sha256:
            fail("Installer payload SHA256 mismatch")
        stream.seek(len(header))
        with tempfile.TemporaryDirectory(prefix="yami-setup-verify-") as temporary:
            root = Path(temporary)
            names, total = set(), 0
            with tarfile.open(fileobj=stream, mode="r|gz") as tar:
                for member in tar:
                    name = member.name
                    if (not member.isreg() or not permitted(name) or member.pax_headers
                            or member.mode not in (0o644, 0o755) or member.uid != 0 or member.gid != 0):
                        fail(f"Unsafe installer member: {name}")
                    key = name.casefold()
                    total += member.size
                    if key in names or len(names) >= MAX_FILES or not 0 < member.size <= MAX_MEMBER or total > MAX_TOTAL:
                        fail(f"Installer payload bounds/collision: {name}")
                    names.add(key)
                    executable = name.startswith("engine/") and name.split("/")[1] in BINARIES
                    executable |= name in {"runtime/" + program for program in RUNTIME_PROGRAMS}
                    if member.mode != (0o755 if executable else 0o644):
                        fail(f"Wrong installer member mode: {name}")
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = tar.extractfile(member)
                    if source is None:
                        fail(f"Unreadable installer member: {name}")
                    with target.open("xb") as destination:
                        shutil.copyfileobj(source, destination)
                    if target.stat().st_size != member.size:
                        fail(f"Truncated installer member: {name}")
            required = {"engine/" + name for name in BINARIES}
            required |= {"runtime/" + name for name in RUNTIME_PROGRAMS} | {PROVENANCE_PATH}
            required |= {"licenses/7zip/" + name for name in LICENSES}
            if not {name.casefold() for name in required}.issubset(names):
                fail("Installer lacks engine/setup/remover/script/archiver/license files")
            if (root / "runtime/uninstall.sh").read_bytes() != UNINSTALL_SCRIPT.read_bytes():
                fail("Installer uninstall script differs from the trusted source")
            if (root / PROVENANCE_PATH).read_bytes() != provenance(arch):
                fail("Unverified 7-Zip source provenance")
            for name, expected in LICENSES.items():
                if digest(root / "licenses/7zip" / name) != expected:
                    fail(f"Unverified 7-Zip license: {name}")
            if expected_engine is not None:
                actual_engine = {path.name: digest(path) for path in (root / "engine").iterdir()}
                if actual_engine != expected_engine:
                    fail("Installer engine differs from the accompanying update ZIP")
            for directory in (root / "engine", root / "runtime"):
                siblings = {path.name for path in directory.iterdir()}
                dependencies = {}
                for path in directory.iterdir():
                    if path.name != "uninstall.sh":
                        dependencies[path.name] = check_elf(path, arch, siblings, glibc_max)
                reachable = set(BINARIES if directory.name == "engine" else RUNTIME_PROGRAMS)
                pending = list(reachable - {"uninstall.sh"})
                while pending:
                    for library in dependencies[pending.pop()] - reachable:
                        reachable.add(library)
                        pending.append(library)
                if siblings != reachable:
                    fail(f"Unexpected files outside the closed {directory.name} dependency set")


def unpack_sevenzip(archive: Path, root: Path, arch: str) -> Path:
    if archive.stat().st_size > 16 * 1024 * 1024 or digest(archive) != SEVENZIP[arch][1]:
        fail("7-Zip archive differs from the pinned official release")
    with tarfile.open(archive, "r:xz") as tar:
        for name in ("7zzs", *LICENSES):
            member = tar.getmember(name)
            if not member.isreg() or not 0 < member.size < 16 * 1024 * 1024:
                fail(f"Unsafe official archiver member: {name}")
            target = root / ("runtime/7zz" if name == "7zzs" else "licenses/7zip/" + name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as destination:
                shutil.copyfileobj(tar.extractfile(member), destination)
            target.chmod(0o755 if name == "7zzs" else 0o644)
    (root / PROVENANCE_PATH).write_bytes(provenance(arch))
    return root / "runtime/7zz"


def package(args) -> None:
    if sys.platform != "linux":
        fail("Installer dependency closure must be collected on native Linux")
    if args.output.name != f"yami-setup-linux-{args.arch}.run" or args.output.exists():
        fail("Expected a new installer asset following the release filename contract")
    verify_archive(args.engine_archive, "linux", args.arch)
    architecture(args.setup_binary, "linux", args.arch)
    remover = args.setup_binary.parent / "yami-remove"
    script = args.setup_binary.parent / "uninstall.sh"
    for source in (args.setup_binary, remover, script):
        if source.is_symlink() or not source.is_file():
            fail(f"Missing regular installer input: {source}")
    if script.read_bytes() != UNINSTALL_SCRIPT.read_bytes():
        fail("Sibling uninstall script differs from the trusted source")
    architecture(remover, "linux", args.arch)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="yami-setup-package-") as temporary:
        root = Path(temporary) / "payload"
        payload = Payload(root / "runtime", "linux", args.arch, extra_paths=("yami-setup", "yami-remove"))
        payload.copy(args.setup_binary, "yami-setup")
        payload.copy(remover, "yami-remove")
        package_linux(payload, [args.setup_binary.resolve(), remover.resolve()], args.linux_glibc_max)
        shutil.copyfile(script, root / "runtime/uninstall.sh")
        (root / "runtime/uninstall.sh").chmod(0o755)
        unpack_sevenzip(args.sevenzip_archive, root, args.arch)
        (root / "engine").mkdir()
        with zipfile.ZipFile(args.engine_archive) as engine:
            for member in engine.infolist():
                target = root / "engine" / member.filename
                with engine.open(member) as source, target.open("xb") as destination:
                    shutil.copyfileobj(source, destination)
                target.chmod(0o755 if member.filename in BINARIES else 0o644)
        compressed = Path(temporary) / "payload.tar.gz"
        with compressed.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                for source in sorted(root.rglob("*")):
                    if source.is_dir():
                        continue
                    name = source.relative_to(root).as_posix()
                    if source.is_symlink() or not source.is_file() or not permitted(name):
                        fail(f"Invalid installer input: {name}")
                    member = tar.gettarinfo(str(source), arcname=name)
                    member.uid = member.gid = member.mtime = 0
                    member.uname = member.gname = ""
                    executable = name.startswith("engine/") and source.name in BINARIES
                    executable |= name.startswith("runtime/") and source.name in RUNTIME_PROGRAMS
                    member.mode = 0o755 if executable else 0o644
                    with source.open("rb") as stream:
                        tar.addfile(member, stream)
        staged = Path(temporary) / args.output.name
        make_sfx(compressed, staged, args.arch, args.version)
        verify_sfx(staged, args.arch, args.linux_glibc_max, args.engine_archive)
        shutil.copyfile(staged, args.output)
        args.output.chmod(0o755)


def regression_checks(archive: Path, arch: str, glibc_max: str | None) -> None:
    """Exercise real artifact corruption rejection without executing its GUI/assets."""
    verify_sfx(archive, arch, glibc_max)
    with archive.open("rb") as stream:
        prefix = stream.read(16384)
    match = MARKER.match(prefix, len(b"#!/bin/sh\n"))
    assert match is not None
    sha, size, _, version = [value.decode() for value in match.groups()]
    header = shell_header(sha, int(size), arch, version)
    with tempfile.TemporaryDirectory(prefix="yami-setup-regression-") as temporary:
        broken = Path(temporary) / archive.name
        pristine = Path(temporary) / "pristine-remove"
        with archive.open("rb") as stream:
            stream.seek(len(header))
            with tarfile.open(fileobj=stream, mode="r|gz") as payload:
                for member in payload:
                    if member.name == "runtime/yami-remove":
                        with payload.extractfile(member) as source, pristine.open("xb") as destination:
                            shutil.copyfileobj(source, destination)
                        break
        if not pristine.is_file():
            fail("Regression: verified installer lacks removal helper")
        other_arch = "arm64" if arch == "x86_64" else "x86_64"
        for index, dependency in enumerate((REMOVER_LOADERS[arch], REMOVER_LOADERS[other_arch],
                                            "libSDL3.so.0", "libGL.so.1")):
            directory = Path(temporary) / f"remover-dependency-{index}"
            directory.mkdir()
            helper = directory / "yami-remove"
            shutil.copyfile(pristine, helper)
            helper.chmod(0o755)
            command("patchelf", "--add-needed", dependency, helper)
            if dependency == REMOVER_LOADERS[arch]:
                check_elf(helper, arch, set(), glibc_max)
                if os.uname().machine in ({"x86_64"} if arch == "x86_64" else {"aarch64", "arm64"}):
                    command(helper, "--help")
                continue
            try:
                check_elf(helper, arch, set(), glibc_max)
            except RuntimeError as error:
                if dependency not in str(error):
                    fail(f"Regression: dependency rejection lacks diagnostic: {dependency}")
            else:
                fail(f"Regression: removal helper accepted unexpected dependency: {dependency}")
        for mutation in ("truncate", "append", "payload", "shell"):
            shutil.copyfile(archive, broken)
            with broken.open("r+b") as stream:
                if mutation == "truncate":
                    stream.truncate(broken.stat().st_size - 1)
                elif mutation == "append":
                    stream.seek(0, os.SEEK_END)
                    stream.write(b"x")
                else:
                    stream.seek(len(header) + 8 if mutation == "payload" else 0)
                    value = stream.read(1)
                    stream.seek(-1, os.SEEK_CUR)
                    stream.write(bytes([value[0] ^ 1]))
            try:
                verify_sfx(broken, arch, glibc_max)
            except RuntimeError:
                pass
            else:
                fail(f"Regression: verifier accepted {mutation}")
            # Corrupt payloads must fail in the actual /bin/sh before tar/GUI launch.
            if mutation != "shell" and os.uname().machine in ({"x86_64"} if arch == "x86_64" else {"aarch64", "arm64"}):
                result = subprocess.run(["/bin/sh", str(broken)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                expected = b"Corrupt payload" if mutation == "payload" else b"Truncated or oversized payload"
                if result.returncode == 0 or expected not in result.stderr:
                    fail(f"Regression: shell did not reject {mutation}: {result.stderr!r}")
        # A rehashed tar is still constrained: traversal and symlink policy cannot be bypassed.
        for name, kind in (("engine/../escape", tarfile.REGTYPE), ("runtime/7zz", tarfile.SYMTYPE)):
            compressed = Path(temporary) / "malformed.tar.gz"
            with tarfile.open(compressed, "w:gz") as tar:
                member = tarfile.TarInfo(name)
                member.type = kind
                member.size = 1 if kind == tarfile.REGTYPE else 0
                member.linkname = "/etc/passwd" if kind == tarfile.SYMTYPE else ""
                tar.addfile(member, io.BytesIO(b"x") if member.size else None)
            broken.unlink()
            make_sfx(compressed, broken, arch, version)
            try:
                verify_sfx(broken, arch, glibc_max)
            except RuntimeError:
                pass
            else:
                fail("Regression: verifier accepted unsafe tar member")
        # Rehash real payloads: support files must remain complete and exact.
        for mutation in ("missing-remover", "missing-script", "extra-helper", "extra-library",
                         "remover-bytes", "script-bytes", "remover-mode", "script-mode"):
            compressed = Path(temporary) / "changed.tar.gz"
            with archive.open("rb") as stream, tarfile.open(compressed, "w:gz", format=tarfile.USTAR_FORMAT) as output:
                stream.seek(len(header))
                with tarfile.open(fileobj=stream, mode="r|gz") as original:
                    for member in original:
                        target = "runtime/uninstall.sh" if "script" in mutation else "runtime/yami-remove"
                        if member.name == target and mutation.startswith("missing-"):
                            continue
                        source = original.extractfile(member)
                        if member.name == target and mutation.endswith("-bytes"):
                            data = source.read()
                            source = io.BytesIO(bytes([data[0] ^ 1]) + data[1:])
                        if member.name == target and mutation.endswith("-mode"):
                            member.mode = 0o644
                        if member.name == "runtime/yami-remove" and mutation.startswith("extra-"):
                            data = source.read()
                            output.addfile(member, io.BytesIO(data))
                            member.name = "runtime/yami-extra" if mutation == "extra-helper" else "runtime/libunowned.so"
                            member.mode = 0o644
                            source = io.BytesIO(data)
                        output.addfile(member, source)
            broken.unlink()
            make_sfx(compressed, broken, arch, version)
            try:
                verify_sfx(broken, arch, glibc_max)
            except RuntimeError:
                pass
            else:
                fail(f"Regression: verifier accepted {mutation}")
    print("Passed installer integrity, shell/path policy, uninstall-support membership/byte/mode and host-loader dependency regressions")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup-binary", type=Path)
    parser.add_argument("--engine-archive", type=Path)
    parser.add_argument("--sevenzip-archive", type=Path)
    parser.add_argument("--arch", choices=tuple(SEVENZIP), required=True)
    parser.add_argument("--version")
    parser.add_argument("--linux-glibc-max")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--self-test", action="store_true", help="Corruption/security regressions against the real --output asset")
    args = parser.parse_args()
    if args.self_test:
        regression_checks(args.output, args.arch, args.linux_glibc_max)
    elif args.verify_only:
        verify_sfx(args.output, args.arch, args.linux_glibc_max, args.engine_archive)
    else:
        if (not args.setup_binary or not args.engine_archive or not args.sevenzip_archive or
                not args.version or not SEMVER.fullmatch(args.version)):
            parser.error("Packaging requires --setup-binary, --engine-archive, --sevenzip-archive and --version MAJOR.MINOR.PATCH")
        package(args)
    print(f"Verified {args.output.name}: sha256:{digest(args.output)}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError, zipfile.BadZipFile, EOFError) as error:
        print(f"Installer packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
