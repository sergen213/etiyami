#!/usr/bin/env python3
"""Regression: installed pre-1.2.1 updaters require a flat engine archive."""
from pathlib import Path
import stat
import tempfile
import zipfile

from package_release import BINARIES, FIDELITYFX_LICENSE, FIDELITYFX_LICENSE_PATH, verify_archive


def write_archive(path: Path, native: bytes, nested_notice: bool) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for name in BINARIES:
            member = zipfile.ZipInfo(name)
            member.external_attr = (stat.S_IFREG | 0o755) << 16
            archive.writestr(member, native if name == "yami-native" else name)
        if nested_notice:
            member = zipfile.ZipInfo(FIDELITYFX_LICENSE_PATH)
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(member, FIDELITYFX_LICENSE.read_bytes())


with tempfile.TemporaryDirectory(prefix="yami-release-check-") as temporary:
    root = Path(temporary)
    flat = root / "flat" / "yami-linux-x86_64.zip"
    flat.parent.mkdir()
    write_archive(flat, FIDELITYFX_LICENSE.read_bytes(), False)
    verify_archive(flat, "linux", "x86_64")

    nested = root / "nested" / "yami-linux-x86_64.zip"
    nested.parent.mkdir()
    write_archive(nested, b"native", True)
    try:
        verify_archive(nested, "linux", "x86_64")
    except RuntimeError:
        pass
    else:
        raise AssertionError("Nested license broke the installed 1.2.0 updater and must be rejected")

print("Flat update archive with embedded FidelityFX notice verified")
