#!/usr/bin/env python3
"""Build/verify a static-runtime SquashFS installer from the verified offline .run."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

from package_setup import MARKER, shell_header, verify_sfx
from package_release import MAX_ARCHIVE, MAX_FILES, MAX_MEMBER, MAX_TOTAL, SEMVER, command, digest, fail

RUNTIME_VERSION = "v0.8.1"
RUNTIME_BASE = f"https://github.com/VHSgunzo/uruntime/releases/download/{RUNTIME_VERSION}/"
# Official release API digests, checked against the actual downloaded static ELFs.
RUNTIMES = {
    "x86_64": ("uruntime-appimage-squashfs-lite-x86_64", 1226208,
               "32c6024a14ff375749ee3bed3330c1586a6dd2bfeddbbd7ceacc053aec2b27a3"),
    "arm64": ("uruntime-appimage-squashfs-lite-aarch64", 1169568,
              "5158dbe45224ecf51aa5ee46be22d4d63f30c6f13a7427bad4af0018016d836c"),
}
# Corresponding helper sources AND build scripts/patches accompany the GPL/LGPL
# static helpers, rather than relying on an upstream source offer or extra assets.
SOURCES = {
    "uruntime": ("VHSgunzo/uruntime", "6ec68e66782d632f22666ba2c661b3afd7c3a565", "0cb935051fbeae7d202a6d17f7298a35e7d7456aa5019d648d8390c1a53b63f8"),
    "squashfs-tools-static": ("VHSgunzo/squashfs-tools-static", "v4.7.5.r2", "2c00afbb6a1f167094f138f72e0326af62550cefbfdd4473c35f454a0cff2104"),
    "squashfuse-static": ("VHSgunzo/squashfuse-static", "v0.6.3.r2", "974c6d19dccf57fcf0017c9250f21dc5d92f8b49a66a7092f4c4e7191ada84d3"),
    "squashfs-tools": ("plougher/squashfs-tools", "708c59ae80853b0845017c33b42e56061cc546cd", "cf0541e42ab1d115bbdde1ea85e2b1f9ccede93133cbd2f64dfe5b8ed5d0666c"),
    "squashfuse": ("vasi/squashfuse", "1a211e20fff55e9ce4c74ad03f0aa26a7b760bd3", "7d795e06806f048d9ffbb5497834d21b34be969ebd1046c4eb9d70ba93639f25"),
    "mimalloc": ("microsoft/mimalloc", "8c532c32c3c96e5ba1f2283e032f69ead8add00f", "5b419de5ed9273f63b8537d26ff9d49549808224036f483589ed8524a56461f7"),
    "libfuse": ("libfuse/libfuse", "033844748010a3b8265bf1c90b9ae8ffe4cd9ca7", "5aeef2f7c616bb0fa03322d034d415b793a2c95a0ba5a9f83125d6dec7259c9f"),
    "xz": ("tukaani-project/xz", "9fc6f5cd8774ebef8d4e030f7081fb6984c0dc3f", "a3267827f68a8ebfa1f023259b64084e094e690ab769a59a210d489cdfd44858"),
    "lzo": ("nemequ/lzo", "0083878c235a89ef96a009d1ff0b500f3a364e4b", "1521328bb6a79d2a863f4ba3de42b6ea26794f9f094d9cdbc813a0948d5f10b1"),
    "zlib": ("madler/zlib", "e3dc0a85b7032e98380dec011bc8f2c2ee0d8fca", "33356dac6140d584347fe46bcf7083bd949dec49ac4b52417ae334ec70e3dbc3"),
    "lz4": ("lz4/lz4", "0774d05537f9762f838f7ab541b7765f1a729cb5", "d0da082c615dd42d8d20c2d46ec2716779e1be05fbf171ac27e2d506ec0e8d7f"),
    "zstd": ("facebook/zstd", "d9c0c7e2cf8a8bf9fb98d3bee546dcf8dc9ac59a", "7063c3a4ea22beae558077cb36d3558eee28c56f10abb93c60b71f27a42f483e"),
    "super-strip": ("aunali1/super-strip", "9c57e288d8b2e0f90c9a15a4223331d1e7b43515", "015e7a30223f42269bf309847806f9ecea484355c499eb8999f5732e47a44d6c"),
}
NOTICES = {
    "musl-COPYRIGHT": ("https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5", "f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af"),
    "rust-LICENSE-MIT": ("https://raw.githubusercontent.com/rust-lang/rust/1.94.0/LICENSE-MIT", "b71bd43a069ca0641a9ecfe585ca7b3c53b5cc1608f8b68321168698e28b5ea1"),
    "rust-LICENSE-APACHE": ("https://raw.githubusercontent.com/rust-lang/rust/1.94.0/LICENSE-APACHE", "62c7a1e35f56406896d7aa7ca52d0cc0d272ac022b5d2796e7d6905db8a3636a"),
    # The two 0.4.0 import-library crates omit license files. This upstream
    # revision still declares both exact versions; retain their own 2016
    # source copyright headers alongside these repository MIT terms.
    "winapi-import-libraries-LICENSE-MIT": (
        "https://raw.githubusercontent.com/retep998/winapi-rs/796a8e6c2971dc2ff1bcff166e6671284f9b5b6b/LICENSE-MIT",
        "ce7bc3499fee93d5022ef430d5e4201e79a6d9154f3974e42f41349f0569e09b"),
    "SPDX-MIT-reference.txt": (
        "https://raw.githubusercontent.com/spdx/license-list-data/d46e94e2c78ceede1cfc63cfa0396472d2798d4c/text/MIT.txt",
        "b05785f9f18e6716bab63424b11454513b9943a222595b70411009202fc592b5"),
}
SOURCE_LIMIT = 32 * 1024 * 1024
# All 84 registry dependencies in the SHA-pinned uruntime Cargo.lock, including
# build/optional/foreign-platform dependencies, receive separate versioned
# attribution. Select permissive alternatives explicitly, not LGPL/GPL options.
CRATE_LICENSES = {
    "MIT": "MIT",
    "MIT OR Apache-2.0": "MIT",
    "MIT/Apache-2.0": "MIT",
    "Apache-2.0 OR MIT": "MIT",
    "0BSD OR MIT OR Apache-2.0": "MIT",
    "MIT OR Zlib OR Apache-2.0": "MIT",
    "MIT OR Apache-2.0 OR LGPL-2.1-or-later": "MIT",
    "Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT": "MIT",
    "(MIT OR Apache-2.0) AND Unicode-3.0": "MIT AND Unicode-3.0",
    "BSD-3-Clause": "BSD-3-Clause",
    "BSL-1.0": "BSL-1.0",
    "Zlib": "Zlib",
}
CRATE_EXTRA_NOTICES = {
    # AUTHORS contains the actual MIT grant and copyright holders, not a
    # substituted generic license, in the exact published r-efi source.
    "r-efi-6.0.0": ("AUTHORS",),
    "winapi-i686-pc-windows-gnu-0.4.0": ("src/lib.rs",),
    "winapi-x86_64-pc-windows-gnu-0.4.0": ("src/lib.rs",),
}
GIT_CRATES = {
    "dotenv": ("0.16.0", "367085af80a8d29d2241c85cbfc25736486230c5",
               "9d31b0d23ef6e1e5851b61feda9a95d4b8ef321af1a7ee737909742d1fcfddd7",
               "dotenv/Cargo.toml", ("LICENSE.md",)),
    "memfd-exec": ("0.2.6", "2decf7d1cec3d183e55000526abd2e5bb6df40c5",
                   "2a0771116e5e9e5acd09b4df7cc88571865d44c7c75fee58447b4b1f81d08492",
                   "Cargo.toml", ()),
}
NOTICE_NAME = re.compile(r"(?:LICENSE|COPYING|COPYRIGHT|NOTICES?)(?:[-._].*)?\Z", re.IGNORECASE)
CRATE_TEXT_LIMIT = 100000
CRATE_TOTAL_LIMIT = 64 * 1024 * 1024
APPRUN = b'''#!/bin/sh
set -eu
# Resolve our own directory, not the caller's cwd; never override host libraries.
appdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$appdir/runtime/yami-setup" "$@"
'''
DESKTOP = b'''[Desktop Entry]
Type=Application
Name=ETI Yami Installer
Comment=Install ETI Yami from your original game ISO
Exec=yami-setup %f
Icon=yami-setup
Terminal=false
Categories=Game;
'''
# Original generic installer/disc artwork, not extracted copyrighted game data.
ICON = b'''<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
<rect x="8" y="8" width="240" height="240" rx="48" fill="#262522"/>
<circle cx="128" cy="116" r="83" fill="#dccca7"/>
<circle cx="128" cy="116" r="61" fill="none" stroke="#a88c55" stroke-width="3"/>
<circle cx="128" cy="116" r="21" fill="#262522"/>
<path d="M115 158h26v35h24l-37 35-37-35h24z" fill="#ab4135"/>
</svg>
'''
LISTING = re.compile(r"([d-][rwx-]{9})\s+0/0\s+([0-9]+)\s+[0-9-]+\s+[0-9:]+\s+squashfs-root(?:/(.*))?\Z")


def runtime_bytes(path: Path, arch: str) -> bytes:
    _, size, sha = RUNTIMES[arch]
    if path.is_symlink() or path.stat().st_size != size:
        fail("Expected the bounded pinned official runtime")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != sha:
        fail("Runtime SHA256 differs from the pinned official release")
    if data[:6] != b"\x7fELF\x02\x01" or data[8:11] != b"AI\x02":
        fail("Runtime is not an ELF64 Type-2 AppImage")
    if struct.unpack_from("<H", data, 18)[0] != {"x86_64": 62, "arm64": 183}[arch]:
        fail("Wrong runtime architecture")
    offset = struct.unpack_from("<Q", data, 32)[0]
    width, count = struct.unpack_from("<HH", data, 54)
    if width != 56 or offset + width * count > len(data):
        fail("Invalid runtime program headers")
    if any(struct.unpack_from("<I", data, offset + width * index)[0] in (2, 3) for index in range(count)):
        fail("Runtime must be fully static (no dynamic segment/interpreter)")
    if data.count(b"URUNTIME_EXTRACT=3") != 1:
        fail("Unrecognized upstream extraction policy")
    # Documented fixed-width policy: FUSE first, unconditional extraction fallback;
    # no requested namespaces/root mapping; upstream cleanup remains enabled.
    return data.replace(b"URUNTIME_EXTRACT=3", b"URUNTIME_EXTRACT=2")


def cached_download(target: Path, url: str, sha: str) -> bytes:
    if not target.exists():
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read(SOURCE_LIMIT + 1)
        if len(data) > SOURCE_LIMIT or hashlib.sha256(data).hexdigest() != sha:
            fail(f"Unverified corresponding source/notice download: {target.name}")
        with target.open("xb") as output:
            output.write(data)
    if target.is_symlink() or not 0 < target.stat().st_size <= SOURCE_LIMIT:
        fail(f"Invalid source cache entry: {target.name}")
    data = target.read_bytes()
    if hashlib.sha256(data).hexdigest() != sha:
        fail(f"Corresponding source/notice SHA256 mismatch: {target.name}")
    return data


def git_crate_files(cache: Path, locked: dict) -> tuple[dict[str, bytes], dict]:
    if set(locked) != set(GIT_CRATES):
        fail("Pinned Cargo.lock git dependencies differ from the reviewed crates")
    files, provenance = {}, {}
    for name, (version, commit, sha, manifest_path, notice_paths) in GIT_CRATES.items():
        repo = f"VHSgunzo/{name}"
        source = f"git+https://github.com/{repo}.git?rev={commit}#{commit}"
        if locked[name].get("version") != version or locked[name].get("source") != source:
            fail(f"Git dependency differs from the pinned exact version/revision: {name}")
        key = f"{name}-{version}"
        url = f"https://codeload.github.com/{repo}/tar.gz/{commit}"
        data = cached_download(cache / f"{key}.tar.gz", url, sha)
        prefix = f"licenses/uruntime/crates/{key}/"
        notices, metadata = {}, {}
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            # Literal reviewed paths only; never extract upstream paths or links.
            for path in (manifest_path, "README.md", *notice_paths):
                member = archive.getmember(f"{name}-{commit}/{path}")
                if not member.isfile() or not 0 < member.size < CRATE_TEXT_LIMIT:
                    fail(f"Expected bounded regular git crate attribution: {key}/{path}")
                text = archive.extractfile(member).read()
                decoded = text.decode("utf-8")
                if any(ord(char) < 32 and char not in "\n\r\t" for char in decoded):
                    fail(f"Non-text git crate attribution: {key}/{path}")
                if path in notice_paths:
                    notices[path] = text
                else:
                    metadata[path] = text
        manifest = dict(re.findall(r'^([a-z]+) = "([^"\n]+)"$', metadata[manifest_path].decode(), re.MULTILINE))
        if manifest.get("name") != name or manifest.get("version") != version or manifest.get("license") != "MIT":
            fail(f"Unreviewed git crate license/version: {key}")
        info = {"name": name, "version": version, "commit": commit, "url": url, "sha256": sha,
                "declared_license": "MIT", "selected_license": "MIT",
                "metadata": {path: hashlib.sha256(text).hexdigest() for path, text in sorted(metadata.items())}}
        if name == "memfd-exec":
            # Neither this exact fork nor original upstream supplies a license
            # file/copyright notice. Preserve their actual source and declaration,
            # never invent an upstream LICENSE or a copyright owner/year.
            notice_url, notice_sha = NOTICES["SPDX-MIT-reference.txt"]
            reference = cached_download(cache / "SPDX-MIT-reference.txt", notice_url, notice_sha)
            permission = reference.partition(b"Permission is hereby granted")[2]
            if not permission:
                fail("Unrecognized pinned SPDX MIT reference")
            notices["SPDX-MIT-permission.txt"] = (
                b"Canonical SPDX MIT permission/disclaimer referenced by upstream Cargo.toml license=\"MIT\".\n"
                b"This is NOT an upstream LICENSE or copyright attribution. Upstream supplies no copyright notice.\n\n"
                b"Permission is hereby granted" + permission)
            files[prefix + "corresponding-source.tar.gz"] = data
            files.update({prefix + "upstream/" + path: text for path, text in metadata.items()})
            info.update({"copyright_notice": "Not supplied by upstream; no holder/year invented",
                         "source_archive": "corresponding-source.tar.gz",
                         "spdx_reference": {"url": notice_url, "sha256": notice_sha}})
        files.update({prefix + path: text for path, text in notices.items()})
        info["notices"] = {path: hashlib.sha256(text).hexdigest() for path, text in sorted(notices.items())}
        provenance[key] = info
    return files, provenance


def crate_files(cache: Path, runtime_source: bytes) -> tuple[dict[str, bytes], dict]:
    root = f"uruntime-{SOURCES['uruntime'][1]}"
    with tarfile.open(fileobj=io.BytesIO(runtime_source), mode="r:gz") as archive:
        member = archive.getmember(f"{root}/Cargo.lock")
        if not member.isfile() or not 0 < member.size < CRATE_TEXT_LIMIT:
            fail("Expected bounded regular pinned Cargo.lock")
        lock = archive.extractfile(member).read().decode("utf-8")
    files, provenance, git_locked = {}, {}, {}
    # Cargo.lock v4 has flat package records. Parse only its quoted scalar
    # fields: Ubuntu 22.04 Python 3.10 has no tomllib, and no TOML dependency
    # is needed for this immutable, SHA-verified upstream lockfile.
    for record in lock.split("[[package]]")[1:]:
        fields = dict(re.findall(r'^([a-z]+) = "([^"\n]+)"$', record, re.MULTILINE))
        if fields.get("source", "").startswith("git+"):
            if fields.get("name") in git_locked:
                fail("Duplicate pinned git dependency")
            git_locked[fields.get("name")] = fields
            continue
        if not fields.get("source", "").startswith("registry+"):
            continue
        name, version, sha = (fields.get(field, "") for field in ("name", "version", "checksum"))
        if (fields["source"] != "registry+https://github.com/rust-lang/crates.io-index"
                or not re.fullmatch(r"[A-Za-z0-9_-]+", name)
                or not re.fullmatch(r"[0-9][A-Za-z0-9.+-]*", version)
                or not re.fullmatch(r"[0-9a-f]{64}", sha)):
            fail("Unrecognized pinned registry dependency")
        key = f"{name}-{version}"
        if key in provenance:
            fail(f"Duplicate pinned registry dependency: {key}")
        url = f"https://static.crates.io/crates/{name}/{key}.crate"
        data = cached_download(cache / f"{key}.crate", url, sha)
        prefix = f"licenses/uruntime/crates/{key}/"
        notices = {}
        license_expression = None
        count, total = 0, 0
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            for member in archive:
                count += 1
                total += member.size
                if count > 4096 or total > CRATE_TOTAL_LIMIT or member.offset_data + member.size > CRATE_TOTAL_LIMIT:
                    fail(f"Registry source archive exceeds bounds: {key}")
                parts = member.name.split("/")
                if (parts[0] != key or len(parts) < 2
                        or any(part in ("", ".", "..") or not re.fullmatch(r"[A-Za-z0-9_.+-]+", part)
                               for part in parts[1:])):
                    fail(f"Unsafe registry source member: {key}")
                relative = "/".join(parts[1:])
                is_manifest = relative == "Cargo.toml"
                is_notice = (NOTICE_NAME.fullmatch(parts[-1]) is not None
                             or relative in CRATE_EXTRA_NOTICES.get(key, ()))
                if not (is_manifest or is_notice):
                    continue
                if not member.isfile() or not 0 < member.size < CRATE_TEXT_LIMIT:
                    fail(f"Expected bounded regular registry attribution: {key}/{relative}")
                text = archive.extractfile(member).read()
                decoded = text.decode("utf-8")
                if any(ord(char) < 32 and char not in "\n\r\t" for char in decoded):
                    fail(f"Non-text registry attribution: {key}/{relative}")
                if is_manifest:
                    if license_expression is not None:
                        fail(f"Duplicate registry manifest: {key}")
                    match = re.search(r'^license = "([^"\n]+)"$', decoded, re.MULTILINE)
                    if not match or match[1] not in CRATE_LICENSES:
                        fail(f"Unreviewed registry license (corresponding source may be required): {key}")
                    license_expression = match[1]
                else:
                    if relative == "src/lib.rs":
                        text = text.partition(b"#!")[0]
                        relative = "COPYRIGHT.txt"
                    if relative in notices:
                        fail(f"Duplicate registry notice: {key}/{relative}")
                    notices[relative] = text
        if key.startswith(("winapi-i686-pc-windows-gnu-", "winapi-x86_64-pc-windows-gnu-")):
            if key not in CRATE_EXTRA_NOTICES or "COPYRIGHT.txt" not in notices:
                fail(f"Unreviewed import-library attribution: {key}")
            notice_name = "winapi-import-libraries-LICENSE-MIT"
            notice_url, notice_sha = NOTICES[notice_name]
            notices["LICENSE-MIT"] = cached_download(cache / notice_name, notice_url, notice_sha)
        if license_expression is None or not notices:
            fail(f"Missing actual registry license/copyright notices: {key}")
        files.update({prefix + path: text for path, text in notices.items()})
        provenance[key] = {
            "name": name, "version": version, "url": url, "sha256": sha,
            "declared_license": license_expression, "selected_license": CRATE_LICENSES[license_expression],
            "notices": {path: hashlib.sha256(text).hexdigest() for path, text in sorted(notices.items())},
        }
        if key.startswith(("winapi-i686-pc-windows-gnu-", "winapi-x86_64-pc-windows-gnu-")):
            provenance[key]["upstream_notice"] = {"url": notice_url, "sha256": notice_sha}
    if len(provenance) != 84:
        fail("Pinned Cargo.lock registry inventory differs from the reviewed 84 crates")
    git_files, git_provenance = git_crate_files(cache, git_locked)
    files.update(git_files)
    provenance.update(git_provenance)
    return files, provenance


def source_files(cache: Path) -> dict[str, bytes]:
    cache.mkdir(parents=True, exist_ok=True)
    files = {}
    for name, (repo, ref, sha) in SOURCES.items():
        target = cache / f"{name}.tar.gz"
        url = f"https://codeload.github.com/{repo}/tar.gz/{ref}"
        data = cached_download(target, url, sha)
        files[f"licenses/uruntime/sources/{name}.tar.gz"] = data
        # Preserve complete source archives without extracting their paths. Publish
        # notices as plain files too, including libfuse's referenced GPL/LGPL terms.
        # Archive member bytes are read, never extracted to upstream paths.
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            for member in archive:
                parts = member.name.split("/")
                if (member.isfile() and 0 < member.size < 100000 and
                        ((len(parts) == 2 and parts[-1].startswith(("COPYING", "LICENSE", "GPL2", "LGPL2")))
                         or (name == "lz4" and parts[1:] == ["lib", "LICENSE"]))):
                    leaf = "-".join(parts[1:])
                    files[f"licenses/uruntime/{name}-{leaf}"] = archive.extractfile(member).read()
    for name, (url, sha) in NOTICES.items():
        files[f"licenses/uruntime/{name}"] = cached_download(cache / name, url, sha)
    crates, provenance = crate_files(cache, files["licenses/uruntime/sources/uruntime.tar.gz"])
    files.update(crates)
    files["licenses/uruntime/crates/provenance.json"] = (
        json.dumps(provenance, indent=2, sort_keys=True) + "\n").encode()
    return files


def prepare_appdir(args, root: Path) -> tuple[bytes, dict[str, tuple[int, int, str]]]:
    # Snapshot caller files before validation, so validated bytes are precisely
    # those subsequently consumed even if the source paths are replaced.
    staging = root.parent / "inputs"
    staging.mkdir()
    setup = staging / args.setup_archive.name
    engine = staging / args.engine_archive.name
    for source, destination in ((args.setup_archive, setup), (args.engine_archive, engine)):
        if source.is_symlink() or not 0 < source.stat().st_size <= MAX_ARCHIVE + 16384:
            fail("Expected bounded regular paired installer/update archives")
        shutil.copyfile(source, destination)
    verify_sfx(setup, args.arch, args.linux_glibc_max, engine)
    with setup.open("rb") as stream:
        match = MARKER.match(stream.read(16384), len(b"#!/bin/sh\n"))
        sha, size, arch, version = [value.decode() for value in match.groups()]
        if version != args.version:
            fail("Installer version differs from requested AppImage version")
        stream.seek(len(shell_header(sha, int(size), arch, version)))
        with tarfile.open(fileobj=stream, mode="r|gz") as archive:
            for member in archive:
                target = root / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, target.open("xb") as destination:
                    shutil.copyfileobj(source, destination)
                target.chmod(member.mode)
    files = source_files(args.source_cache)
    runtime = runtime_bytes(args.runtime_file, args.arch)
    runtime_name, runtime_size, runtime_sha = RUNTIMES[args.arch]
    provenance = {
        "version": args.version, "arch": args.arch, "runtime_version": RUNTIME_VERSION,
        "runtime_url": RUNTIME_BASE + runtime_name, "runtime_sha256": runtime_sha,
        "runtime_size": runtime_size, "configured_runtime_sha256": hashlib.sha256(runtime).hexdigest(),
        "release_api": f"https://api.github.com/repos/VHSgunzo/uruntime/releases/tags/{RUNTIME_VERSION}",
        "policy": "URUNTIME_EXTRACT=2; URUNTIME_UNSHARE=0; URUNTIME_CLEANUP=1",
        "setup_sha256": digest(setup), "engine_sha256": digest(engine),
        "sources": {name: {"url": f"https://codeload.github.com/{repo}/tar.gz/{ref}", "sha256": sha}
                    for name, (repo, ref, sha) in SOURCES.items()},
        "notices": {name: {"url": url, "sha256": sha} for name, (url, sha) in NOTICES.items()},
        "cargo_dependencies": json.loads(files["licenses/uruntime/crates/provenance.json"]),
    }
    files.update({"AppRun": APPRUN, "yami-setup.desktop": DESKTOP, "yami-setup.svg": ICON,
                  ".DirIcon": ICON, "licenses/uruntime/provenance.json":
                  (json.dumps(provenance, indent=2, sort_keys=True) + "\n").encode()})
    for name, data in files.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o755 if name == "AppRun" else 0o644)
    expected = {}
    total = 0
    for path in root.rglob("*"):
        name = path.relative_to(root).as_posix()
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            fail(f"Unsafe AppDir member: {name}")
        if path.is_dir():
            path.chmod(0o755)
            expected[name] = (0o755, 0, "")
        else:
            size = path.stat().st_size
            total += size
            if not 0 < size <= MAX_MEMBER or total > MAX_TOTAL:
                fail("AppDir exceeds payload bounds")
            expected[name] = (path.stat().st_mode & 0o777, size, digest(path))
    if len(expected) > MAX_FILES:
        fail("AppDir exceeds member bounds")
    root.chmod(0o755)
    expected[""] = (0o755, 0, "")
    return runtime, expected


def squash(root: Path, target: Path) -> None:
    command("mksquashfs", root, target, "-noappend", "-all-root", "-no-xattrs", "-no-exports",
            "-nopad", "-comp", "gzip", "-mkfs-time", "0", "-all-time", "0", "-processors", "2", "-no-progress")


def verify_image(image: Path, runtime: bytes, expected: dict[str, tuple[int, int, str]], scratch: Path) -> None:
    if image.is_symlink() or not 0 < image.stat().st_size <= MAX_ARCHIVE + len(runtime):
        fail("Expected bounded regular AppImage")
    squashfs = scratch / "payload.squashfs"
    with image.open("rb") as stream:
        if stream.read(len(runtime)) != runtime:
            fail("AppImage runtime/policy differs from trusted configured release")
        superblock = stream.read(96)
        if len(superblock) != 96 or superblock[:4] != b"hsqs" or struct.unpack_from("<HH", superblock, 28) != (4, 0):
            fail("Missing SquashFS v4 payload")
        if struct.unpack_from("<I", superblock, 4)[0] != len(expected):
            fail("SquashFS inode inventory exceeds/differs from the bounded trusted payload")
        if struct.unpack_from("<Q", superblock, 56)[0] != 0xffffffffffffffff:
            fail("SquashFS extended attributes are not permitted")
        used = struct.unpack_from("<Q", superblock, 40)[0]
        if used != image.stat().st_size - len(runtime):
            fail("Truncated/trailing SquashFS data")
        with squashfs.open("xb") as target:
            target.write(superblock)
            shutil.copyfileobj(stream, target)
    seen = set()
    for line in command("unsquashfs", "-lln", squashfs).splitlines():
        match = LISTING.fullmatch(line)
        if not match:
            fail(f"Unsafe/unrecognized SquashFS inventory: {line}")
        mode, size, name = match.groups()
        name = name or ""
        if name in seen or name not in expected:
            fail(f"Unexpected/colliding SquashFS member: {name}")
        seen.add(name)
        permissions, length, sha = expected[name]
        required_mode = "drwxr-xr-x" if not sha else ("-rwxr-xr-x" if permissions == 0o755 else "-rw-r--r--")
        if mode != required_mode or (sha and int(size) != length):
            fail(f"Wrong SquashFS type/mode/size: {name}")
    if seen != set(expected):
        fail("SquashFS inventory differs from verified installer/AppDir")
    # No unknown paths, links, special files, xattrs, owners, or oversized files
    # survive the inventory gate. Never extract an arbitrary caller's AppDir.
    extracted = scratch / "extracted"
    command("unsquashfs", "-no-progress", "-no-xattrs", "-processors", "2", "-dest", extracted, squashfs)
    actual = {path.relative_to(extracted).as_posix() for path in extracted.rglob("*")}
    if actual != set(expected) - {""}:
        fail("Extracted SquashFS inventory mismatch")
    for name, (_, size, sha) in expected.items():
        if sha:
            path = extracted / name
            if path.is_symlink() or path.stat().st_size != size or digest(path) != sha:
                fail(f"AppImage bytes differ from verified paired archives: {name}")


def regression_checks(args, runtime: bytes, expected, root: Path) -> None:
    for mutation in ("truncate", "append", "runtime", "policy", "filesystem"):
        folder = root / mutation
        folder.mkdir()
        broken = folder / args.output.name
        shutil.copyfile(args.output, broken)
        with broken.open("r+b") as stream:
            if mutation == "truncate":
                stream.truncate(broken.stat().st_size - 1)
            elif mutation == "append":
                stream.seek(0, os.SEEK_END)
                stream.write(b"x")
            else:
                offset = {"runtime": 0, "policy": runtime.index(b"URUNTIME_EXTRACT=2") + 17,
                          "filesystem": len(runtime)}[mutation]
                stream.seek(offset)
                value = stream.read(1)
                stream.seek(offset)
                stream.write(bytes([value[0] ^ 1]))
        try:
            verify_image(broken, runtime, expected, folder)
        except RuntimeError:
            pass
        else:
            fail(f"Regression: accepted AppImage {mutation} corruption")
    appdir = root / "AppDir"
    apprun = appdir / "AppRun"
    for mutation in ("copyright-member", "symlink", "changed-payload", "crate-notice", "crate-provenance",
                     "missing-remover", "remover-bytes", "script-bytes"):
        folder = root / mutation
        folder.mkdir()
        changed = appdir / {
            "crate-notice": "licenses/uruntime/crates/goblin-0.10.7/LICENSE",
            "crate-provenance": "licenses/uruntime/crates/provenance.json",
            "missing-remover": "runtime/yami-remove",
            "remover-bytes": "runtime/yami-remove",
            "script-bytes": "runtime/uninstall.sh",
        }.get(mutation, "AppRun")
        original = changed.read_bytes()
        original_mode = changed.stat().st_mode & 0o777
        if mutation == "copyright-member":
            (appdir / "game.ini").write_bytes(b"unexpected original-game data")
        elif mutation == "missing-remover":
            changed.unlink()
        elif mutation == "symlink":
            apprun.unlink()
            apprun.symlink_to("/etc/passwd")
        else:
            changed.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        filesystem = folder / "malformed.squashfs"
        squash(appdir, filesystem)
        broken = folder / args.output.name
        with broken.open("xb") as target, filesystem.open("rb") as source:
            target.write(runtime)
            shutil.copyfileobj(source, target)
        try:
            verify_image(broken, runtime, expected, folder)
        except RuntimeError:
            pass
        else:
            fail(f"Regression: accepted AppImage {mutation}")
        if mutation == "copyright-member":
            (appdir / "game.ini").unlink()
        elif mutation == "symlink":
            apprun.unlink()
            apprun.write_bytes(APPRUN)
            apprun.chmod(0o755)
        else:
            changed.write_bytes(original)
            changed.chmod(original_mode)
    print("Passed AppImage runtime/policy, filesystem, bounds, symlink, unexpected-member, payload-byte and crate-attribution regressions")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup-archive", type=Path, required=True)
    parser.add_argument("--engine-archive", type=Path, required=True)
    parser.add_argument("--runtime-file", type=Path, required=True)
    parser.add_argument("--source-cache", type=Path)
    parser.add_argument("--arch", choices=tuple(RUNTIMES), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--linux-glibc-max")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not SEMVER.fullmatch(args.version) or args.output.name != f"yami-setup-linux-{args.arch}.AppImage":
        parser.error("Expected stable MAJOR.MINOR.PATCH and architecture-specific AppImage release filename")
    if args.source_cache is None:
        args.source_cache = args.runtime_file.parent / "uruntime-sources"
    if not (args.verify_only or args.self_test) and args.output.exists():
        fail("Refusing to overwrite an existing AppImage")
    with tempfile.TemporaryDirectory(prefix="yami-appimage-") as temporary:
        root = Path(temporary)
        appdir = root / "AppDir"
        appdir.mkdir()
        runtime, expected = prepare_appdir(args, appdir)
        verification = root / "verification"
        verification.mkdir()
        if args.verify_only or args.self_test:
            verify_image(args.output, runtime, expected, verification)
            if args.self_test:
                regression_checks(args, runtime, expected, root)
        else:
            filesystem = root / "installer.squashfs"
            squash(appdir, filesystem)
            staged = root / args.output.name
            with staged.open("xb") as target, filesystem.open("rb") as source:
                target.write(runtime)
                shutil.copyfileobj(source, target)
            staged.chmod(0o755)
            verify_image(staged, runtime, expected, verification)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with staged.open("rb") as source, args.output.open("xb") as target:
                shutil.copyfileobj(source, target)
            args.output.chmod(0o755)
    print(f"Verified {args.output.name}: sha256:{digest(args.output)}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError, zipfile.BadZipFile, EOFError) as error:
        print(f"AppImage packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
