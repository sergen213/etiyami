#!/usr/bin/env python3
"""Package a clean CMake install tree, never a source/build/game directory.

Linux requires ldd/readelf/patchelf; macOS requires Xcode command-line tools.
Windows PE imports (including delay imports) are read with the Python stdlib.
Use --verify-only to validate an existing release ZIP without platform tools.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

BINARIES = ("yami-native", "yami-updater", "yami-launcher")
SEMVER = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")
# These belong to the host OS/driver stack, not a redistributable engine runtime.
# Host GPU drivers can require newer C++/unwind symbols than the build distro.
LINUX_SYSTEM = re.compile(
    r"(?:ld-linux[^/]*|ld64[^/]*|linux-vdso[^/]*|"
    r"lib(?:c|m|dl|pthread|rt|resolv|util|anl|nss_[^.]+|stdc\+\+|gcc_s)\.so(?:\.[0-9]+)*|"
    r"lib(?:GL|GLX|GLdispatch|OpenGL|EGL|GLESv[12]|vulkan|drm(?:_[^.]+)?|gbm|"
    r"cuda|nvidia[^.]*|vdpau|va(?:-x11|-drm|-wayland)?)\.so(?:\.[0-9]+)*)\Z"
)
WINDOWS_SYSTEM = {name + ".dll" for name in (
    "advapi32", "avrt", "bcrypt", "bcryptprimitives", "cfgmgr32", "combase", "comctl32",
    "comdlg32", "crypt32", "cryptbase", "d3d11", "d3d12", "d3d9", "dcomp", "dbghelp",
    "dnsapi", "dsound", "dwmapi", "dxgi", "dxva2", "gdi32", "hid", "imm32", "iphlpapi",
    "kernel32", "kernelbase", "mf", "mfplat", "mfreadwrite", "mfuuid", "msvcrt", "ncrypt",
    "netapi32", "normaliz", "ntdll", "ole32", "oleaut32", "opengl32", "powrprof", "propsys",
    "psapi", "rpcrt4", "secur32", "setupapi", "shell32", "shlwapi", "ucrtbase", "user32",
    "userenv", "usp10", "uxtheme", "version", "winhttp", "wininet", "winmm", "winspool",
    "wintrust", "wldap32", "ws2_32", "wtsapi32"
)}
MAX_MEMBER = 512 * 1024 * 1024
MAX_TOTAL = 2 * 1024 * 1024 * 1024
MAX_ARCHIVE = 1024 * 1024 * 1024
MAX_FILES = 8192


def windows_system(name: str) -> bool:
    return (name in WINDOWS_SYSTEM or name.startswith(("api-ms-win-", "ext-ms-win-"))
            or (Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32" / name).is_file())


def fail(message: str) -> None:
    raise RuntimeError(message)


def command(*args: str | Path) -> str:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    # Dependency discovery must not accidentally use a developer's loader overrides.
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES",
                 "GH_TOKEN", "GITHUB_TOKEN"):
        environment.pop(name, None)
    result = subprocess.run([str(arg) for arg in args], check=True, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
    return result.stdout


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() if sys.version_info >= (3, 11) else hash_stream(stream)


def hash_stream(stream) -> str:
    value = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        value.update(chunk)
    return value.hexdigest()


def required_paths(platform: str) -> tuple[str, ...]:
    if platform == "windows":
        return tuple(name + ".exe" for name in BINARIES)
    if platform == "macos":
        return ("yami-native", "yami-updater", "yami-launcher.app/Contents/MacOS/yami-launcher")
    return BINARIES


def allowed_path(name: str, platform: str) -> bool:
    parts = PurePosixPath(name).parts
    if not parts or name != "/".join(parts) or name.startswith("/"):
        return False
    if any(not re.fullmatch(r"[A-Za-z0-9_+.,@() -]+", part) or part in (".", "..")
           or part.endswith((".", " "))
           or re.fullmatch(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])", part.split(".")[0], re.IGNORECASE)
           for part in parts):
        return False
    if any(part.lower() in ("game", ".git", "game.ini", "checkpoints")
           or re.fullmatch(r"save[0-9]+\.eti", part.lower()) for part in parts):
        return False
    if name in required_paths(platform):
        return True
    library = parts[-1]
    library_ok = {
        "linux": bool(re.fullmatch(r"lib[^/]+\.so(?:\.[0-9]+)*", library)),
        "windows": library.lower().endswith(".dll"),
        "macos": bool(re.fullmatch(r"lib[^/]+\.dylib", library)),
    }[platform]
    if library_ok and (len(parts) == 1 or (len(parts) == 2 and parts[0] == "lib")):
        return True
    if platform != "macos" or parts[:2] != ("yami-launcher.app", "Contents"):
        return False
    rest = parts[2:]
    return (rest in (("Info.plist",), ("PkgInfo",), ("_CodeSignature", "CodeResources"))
            or (len(rest) == 2 and rest[0] == "Resources"
                and (rest[1].endswith(".icns") or rest[1] in ("Assets.car", "native-release.txt")))
            or (len(rest) == 2 and rest[0] == "Frameworks" and library_ok))


def architecture(path: Path, platform: str, arch: str) -> None:
    if platform == "macos":
        if arch not in command("lipo", "-archs", path).split():
            fail(f"Wrong architecture: {path}")
        return
    with path.open("rb") as stream:
        header = stream.read(64)
        if platform == "linux":
            if header[:6] != b"\x7fELF\x02\x01" or len(header) < 20:
                fail(f"Not a little-endian ELF64 binary: {path}")
            machine = struct.unpack_from("<H", header, 18)[0]
            expected = {"x86_64": 62, "arm64": 183}[arch]
        else:
            if header[:2] != b"MZ" or len(header) < 64:
                fail(f"Not a Windows binary: {path}")
            stream.seek(struct.unpack_from("<I", header, 60)[0])
            pe = stream.read(6)
            if pe[:4] != b"PE\0\0" or len(pe) != 6:
                fail(f"Invalid PE header: {path}")
            machine = struct.unpack_from("<H", pe, 4)[0]
            expected = {"x86_64": 0x8664, "arm64": 0xAA64}[arch]
        if machine != expected:
            fail(f"Wrong architecture: {path}")


def pe_imports(path: Path) -> set[str]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 60)[0]
    section_count, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        fail(f"Expected PE32+: {path}")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    directories = optional + 112
    sections = []
    for index in range(section_count):
        entry = optional + optional_size + 40 * index
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from("<IIII", data, entry + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_address, raw_size))

    def offset(rva: int) -> int:
        for address, size, raw, raw_size in sections:
            if address <= rva < address + size and rva - address < raw_size:
                return raw + rva - address
        fail(f"Unmapped PE address in {path}: {rva}")
        return 0

    imports = set()
    # IMAGE_IMPORT_DESCRIPTOR and IMAGE_DELAYLOAD_DESCRIPTOR name fields.
    for directory_index, width, name_offset in ((1, 20, 12), (13, 32, 4)):
        rva, size = struct.unpack_from("<II", data, directories + 8 * directory_index)
        if not rva:
            continue
        start = offset(rva)
        for cursor in range(start, start + size, width):
            descriptor = data[cursor:cursor + width]
            if len(descriptor) != width:
                fail(f"Truncated PE imports: {path}")
            if not any(descriptor):
                break
            name_rva = struct.unpack_from("<I", descriptor, name_offset)[0]
            if directory_index == 13 and not (struct.unpack_from("<I", descriptor)[0] & 1):
                name_rva -= image_base
            name_start = offset(name_rva)
            name_end = data.find(b"\0", name_start, name_start + 256)
            if name_end < 0:
                fail(f"Invalid PE import name: {path}")
            name = data[name_start:name_end].decode("ascii")
            if not re.fullmatch(r"[A-Za-z0-9_+.-]+\.dll", name, re.IGNORECASE):
                fail(f"Unsafe PE import name in {path}: {name}")
            imports.add(name)
    return imports


def linux_dependencies(path: Path) -> list[tuple[str, Path]]:
    needed = set(command("patchelf", "--print-needed", path).splitlines())
    if any("/" in name for name in needed):
        fail(f"Absolute ELF dependency in {path}")
    wanted = {name for name in needed if not LINUX_SYSTEM.fullmatch(name)}
    text = command("ldd", path)
    if "not found" in text:
        fail(f"Unresolved runtime dependency of {path}:\n{text}")
    dependencies = []
    for line in text.splitlines():
        match = re.match(r"\s*(\S+) => (/.*?)\s+\(0x[0-9a-f]+\)", line)
        if match and match[1] in wanted:
            dependencies.append((match[1], Path(match[2])))
    missing = wanted - {name for name, _ in dependencies}
    if missing:
        fail(f"Unresolved direct ELF imports in {path}: {sorted(missing)}")
    return dependencies


def mac_rpaths(path: Path) -> list[str]:
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)", command("otool", "-l", path))


def mac_dependencies(path: Path, executable: Path) -> list[tuple[str, Path]]:
    dependencies = []
    own_id = command("otool", "-D", path).splitlines()[1:]
    for line in command("otool", "-L", path).splitlines()[1:]:
        name = line.strip().split(" (compatibility version", 1)[0]
        if name in own_id or name.startswith(("/System/Library/", "/usr/lib/")):
            continue

        def expand(value: str) -> Path:
            return Path(value.replace("@loader_path", str(path.parent))
                        .replace("@executable_path", str(executable.parent)))

        if name.startswith("@rpath/"):
            tail = name[len("@rpath/"):]
            candidates = [expand(rpath) / tail for rpath in mac_rpaths(path) + mac_rpaths(executable)]
        else:
            candidates = [expand(name)]
        resolved = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
        if resolved is None:
            fail(f"Unresolved Mach-O dependency in {path}: {name}")
        if resolved.suffix != ".dylib":
            fail(f"Non-system framework requires explicit packaging support: {resolved}")
        dependencies.append((name, resolved))
    return dependencies


class Payload:
    def __init__(self, root: Path, platform: str, arch: str, *, extra_paths: tuple[str, ...] = ()):
        self.root, self.platform, self.arch = root, platform, arch
        self.extra_paths = extra_paths
        self.sources: dict[str, Path] = {}
        self.names: dict[str, str] = {}

    def copy(self, source: Path, name: str) -> tuple[Path, bool]:
        if not allowed_path(name, self.platform) and name not in self.extra_paths:
            fail(f"Not an update-owned path: {name}")
        source = source.resolve(strict=True)
        if not source.is_file() or source.stat().st_size > MAX_MEMBER:
            fail(f"Not a bounded regular runtime file: {source}")
        key = name.casefold()
        if key in self.names:
            previous = self.names[key]
            if previous != name or self.sources[previous] != source:
                fail(f"Colliding runtime libraries at {name}: {self.sources[previous]} / {source}")
            return self.root / previous, False
        self.names[key] = name
        self.sources[name] = source
        destination = self.root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        destination.chmod(0o755 if source.stat().st_mode & 0o111 else 0o644)
        return destination, True


def package_linux(payload: Payload, executables: list[Path], glibc_max: str | None) -> None:
    pending = list(executables)
    visited = set()
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        visited.add(source)
        for name, dependency in linux_dependencies(source):
            _, added = payload.copy(dependency, name)
            if added:
                pending.append(dependency.resolve())
    for name in payload.sources:
        target = payload.root / name
        architecture(target, "linux", payload.arch)
        if glibc_max:
            maximum = tuple(map(int, glibc_max.split(".")))
            versions = re.findall(r"\bGLIBC_([0-9]+\.[0-9]+)(?:\.[0-9]+)?\b", command("readelf", "--version-info", target))
            if any(tuple(map(int, version.split("."))) > maximum for version in versions):
                fail(f"{target.name} needs glibc newer than {glibc_max}")
        needed = command("patchelf", "--print-needed", target).splitlines()
        if any("/" in library for library in needed):
            fail(f"Absolute ELF dependency in {target}")
        # Flat runtime deliberately leaves legacy lib/ out of the loader search path.
        command("patchelf", "--set-rpath", "$ORIGIN", target)
    for name in payload.sources:
        target = payload.root / name
        for library, dependency in linux_dependencies(target):
            if dependency.resolve().parent != payload.root.resolve():
                fail(f"Runtime still escapes package: {target.name} -> {library}: {dependency}")


def package_windows(payload: Payload, executables: list[Path], search_dirs: list[Path]) -> None:
    libraries: dict[str, Path] = {}
    for directory in search_dirs:
        for path in directory.iterdir():
            if path.is_file() and path.suffix.lower() == ".dll":
                key = path.name.casefold()
                if key in libraries and libraries[key].resolve() != path.resolve():
                    fail(f"Ambiguous DLL search path: {path.name}")
                libraries[key] = path
    pending = list(executables)
    visited = set()
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        visited.add(source)
        architecture(source, "windows", payload.arch)
        for name in pe_imports(source):
            key = name.casefold()
            if windows_system(key):
                continue
            if key not in libraries:
                fail(f"Unresolved non-system Windows import: {source.name} -> {name}")
            dependency = libraries[key]
            _, added = payload.copy(dependency, dependency.name)
            if added:
                pending.append(dependency.resolve())
    for name in payload.sources:
        architecture(payload.root / name, "windows", payload.arch)
        for imported in pe_imports(payload.root / name):
            key = imported.casefold()
            if key not in payload.names and not windows_system(key):
                fail(f"Incomplete Windows dependency closure: {name} -> {imported}")


def package_macos(payload: Payload, executables: list[Path], version: str, minimum: str) -> None:
    pending = [(path, path) for path in executables]
    dependencies: dict[Path, list[tuple[str, Path]]] = {}
    while pending:
        source, executable = pending.pop()
        if source in dependencies:
            continue
        architecture(source, "macos", payload.arch)
        dependencies[source] = mac_dependencies(source, executable)
        for _, dependency in dependencies[source]:
            payload.copy(dependency, "lib/" + dependency.name)
            pending.append((dependency, executable))
    for name, source in payload.sources.items():
        if source not in dependencies:
            continue
        target = payload.root / name
        load_commands = command("otool", "-l", target)
        versions = re.findall(r"\bminos ([0-9.]+)", load_commands)
        versions += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([0-9.]+)", load_commands)
        def os_version(value: str) -> tuple[int, ...]:
            return tuple((list(map(int, value.split("."))) + [0, 0, 0])[:3])
        if not versions or any(os_version(value) > os_version(minimum) for value in versions):
            fail(f"{target.name} requires macOS newer than {minimum}, or has no deployment target")
        for old_name, dependency in dependencies[source]:
            relative = os.path.relpath(payload.root / "lib" / dependency.name, target.parent)
            command("install_name_tool", "-change", old_name, "@loader_path/" + relative, target)
        if name.startswith("lib/"):
            command("install_name_tool", "-id", "@loader_path/" + target.name, target)
        for rpath in set(mac_rpaths(target)):
            command("install_name_tool", "-delete_rpath", rpath, target)
    app = payload.root / "yami-launcher.app"
    info_path = app / "Contents/Info.plist"
    with info_path.open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleExecutable") != "yami-launcher":
        fail("Unexpected launcher CFBundleExecutable")
    info.update(CFBundleShortVersionString=version, CFBundleVersion=version, LSMinimumSystemVersion=minimum)
    with info_path.open("wb") as stream:
        plistlib.dump(info, stream)
    (app / "Contents/Resources").mkdir(exist_ok=True)
    (app / "Contents/Resources/native-release.txt").write_text(
        f"ETI Yami {version}\nmacos-{payload.arch}\n", encoding="ascii")
    for name, source in payload.sources.items():
        if source in dependencies:
            target = payload.root / name
            for _, dependency in mac_dependencies(target, target):
                if dependency.parent != (payload.root / "lib").resolve():
                    fail(f"Mach-O runtime escapes package: {target}")
            command("codesign", "--force", "--sign", "-", target)
            command("codesign", "--verify", "--strict", target)
    command("codesign", "--force", "--sign", "-", app)
    command("codesign", "--verify", "--deep", "--strict", app)


def verify_archive(archive: Path, platform: str, arch: str) -> None:
    expected_name = f"yami-{platform}-{arch}.zip"
    if archive.name != expected_name or archive.stat().st_size > MAX_ARCHIVE:
        fail(f"Expected bounded release asset {expected_name}")
    with zipfile.ZipFile(archive) as zip_file:
        files = zip_file.infolist()
        if not files or len(files) > MAX_FILES:
            fail("Invalid archive member count")
        names, total = set(), 0
        for member in files:
            mode = member.external_attr >> 16
            if (member.is_dir() or not stat.S_ISREG(mode) or member.flag_bits & 1
                    or member.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                    or not allowed_path(member.filename, platform)):
                fail(f"Unsafe archive member: {member.filename}")
            if platform == "linux" and ("/" in member.filename or LINUX_SYSTEM.fullmatch(member.filename)):
                fail(f"Linux archive must contain only flat application runtime: {member.filename}")
            key = member.filename.casefold()
            if key in names:
                fail(f"Case-folded path collision: {member.filename}")
            names.add(key)
            total += member.file_size
            if member.file_size == 0 or member.file_size > MAX_MEMBER or total > MAX_TOTAL:
                fail(f"Archive payload bounds exceeded: {member.filename}")
        if not set(required_paths(platform)).issubset({member.filename for member in files}):
            fail("Archive omits a required engine binary")
        if platform != "windows":
            by_name = {member.filename: member for member in files}
            if any(not ((by_name[name].external_attr >> 16) & 0o111) for name in required_paths(platform)):
                fail("Archive engine binaries are not executable")
        bad = zip_file.testzip()
        if bad:
            fail(f"Archive CRC check failed: {bad}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "macos"), required=True)
    parser.add_argument("--arch", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--version")
    parser.add_argument("--search-dir", type=Path, action="append", default=[])
    parser.add_argument("--linux-glibc-max", help="Release CI uses 2.35; omit for local packaging exercises")
    parser.add_argument("--macos-min", default="15.0")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if args.verify_only:
        verify_archive(args.output, args.platform, args.arch)
        print(f"Verified {args.output.name}: sha256:{digest(args.output)}")
        return
    if args.install_root is None or args.version is None or not SEMVER.fullmatch(args.version):
        parser.error("Packaging requires --install-root and --version MAJOR.MINOR.PATCH")
    native_platform = {"linux": "linux", "win32": "windows", "darwin": "macos"}.get(sys.platform)
    if native_platform != args.platform:
        parser.error("Dependency closure must be collected on the target platform")
    install = args.install_root.resolve(strict=True)
    if not install.is_dir():
        fail("Install root is not a directory")
    expected = set(required_paths(args.platform))
    existing = []
    for path in install.rglob("*"):
        if path.is_symlink():
            fail(f"Install tree contains a symlink: {path}")
        if path.is_dir():
            relative = path.relative_to(install).parts
            if any(part.casefold() in ("game", ".git", "checkpoints") for part in relative):
                fail(f"Protected directory in install tree: {path}")
            continue
        name = path.relative_to(install).as_posix()
        if not path.is_file() or not allowed_path(name, args.platform):
            fail(f"Unexpected file in clean install tree: {name}")
        existing.append((path, name))
    if not expected.issubset({name for _, name in existing}):
        fail("CMake install tree is missing one of the three engine binaries")
    if args.platform == "macos" and not (install / "yami-launcher.app/Contents/Info.plist").is_file():
        fail("CMake did not install the macOS launcher Info.plist")
    output = args.output.absolute()
    if output.name != f"yami-{args.platform}-{args.arch}.zip":
        fail("Output filename must follow the release asset contract")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        fail(f"Refusing to replace an existing release asset: {output}")
    with tempfile.TemporaryDirectory(prefix="yami-package-") as temporary:
        payload = Payload(Path(temporary) / "payload", args.platform, args.arch)
        for source, name in existing:
            if args.platform == "linux" and name not in expected:
                if LINUX_SYSTEM.fullmatch(source.name):
                    continue
                name = source.name
            payload.copy(source, name)
        executables = [install / name for name in required_paths(args.platform)]
        for executable in executables:
            architecture(executable, args.platform, args.arch)
        if args.platform == "linux":
            package_linux(payload, executables, args.linux_glibc_max)
        elif args.platform == "windows":
            if not args.search_dir:
                parser.error("Windows packaging requires --search-dir pointing at the matching MSYS2 bin directory")
            package_windows(payload, executables, [install] + args.search_dir)
        else:
            package_macos(payload, executables, args.version, args.macos_min)
        temporary_archive = Path(temporary) / output.name
        with zipfile.ZipFile(temporary_archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zip_file:
            for source in sorted(payload.root.rglob("*")):
                if source.is_dir():
                    continue
                name = source.relative_to(payload.root).as_posix()
                if source.is_symlink() or not source.is_file() or not allowed_path(name, args.platform):
                    fail(f"Invalid final payload member: {name}")
                zip_file.write(source, name)
        verify_archive(temporary_archive, args.platform, args.arch)
        shutil.copyfile(temporary_archive, output)
    print(f"Packaged {output.name}: sha256:{digest(output)}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, struct.error, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"Release packaging failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
