#!/usr/bin/env python3
"""Regression: installed pre-1.2.1 updaters require a flat engine archive."""
from pathlib import Path
import stat
import tempfile
import zipfile

from package_release import BINARIES, FIDELITYFX_LICENSE, FIDELITYFX_LICENSE_PATH, verify_archive


def write_archive(path: Path, native: bytes, nested_notice: bool, platform: str = "linux", nested_library: bool = False) -> None:
    suffix = ".exe" if platform == "windows" else ""
    with zipfile.ZipFile(path, "w") as archive:
        for name in BINARIES:
            member = zipfile.ZipInfo(name + suffix)
            member.external_attr = (stat.S_IFREG | 0o755) << 16
            archive.writestr(member, native if name == "yami-native" else name)
        if nested_notice:
            member = zipfile.ZipInfo(FIDELITYFX_LICENSE_PATH)
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(member, FIDELITYFX_LICENSE.read_bytes())
        if nested_library:
            member = zipfile.ZipInfo("lib/zz-check.dll")
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(member, b"nested")


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
    windows = root / "windows" / "yami-windows-x86_64.zip"
    windows.parent.mkdir()
    write_archive(windows, FIDELITYFX_LICENSE.read_bytes(), False, "windows")
    verify_archive(windows, "windows", "x86_64")

    missing_notice = root / "windows-missing" / "yami-windows-x86_64.zip"
    missing_notice.parent.mkdir()
    write_archive(missing_notice, b"native", False, "windows")
    try:
        verify_archive(missing_notice, "windows", "x86_64")
    except RuntimeError:
        pass
    else:
        raise AssertionError("Windows native engine must embed the FidelityFX notice")
    nested_library = root / "windows-nested-library" / "yami-windows-x86_64.zip"
    nested_library.parent.mkdir()
    write_archive(nested_library, FIDELITYFX_LICENSE.read_bytes(), False, "windows", True)
    try:
        verify_archive(nested_library, "windows", "x86_64")
    except RuntimeError:
        pass
    else:
        raise AssertionError("Windows update archives must keep every DLL at the root")

print("Flat Linux/Windows update archives with embedded FidelityFX notices verified")
