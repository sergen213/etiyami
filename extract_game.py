#!/usr/bin/env python3
"""Extract this Yami MSI/CAB without executing its legacy installers."""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def payload_paths():
    def stream(name):
        return subprocess.check_output(
            ['7z', 'x', '-so', str(ROOT / 'Yami.msi'), '!' + name],
            stderr=subprocess.DEVNULL,
        )

    pool, data = stream('_StringPool'), stream('_StringData')
    codepage = struct.unpack_from('<I', pool)[0]
    if codepage != 1254:
        raise ValueError('Unsupported MSI string-pool format; expected this Turkish Yami release')
    strings, offset = [''], 0
    for length, _ in struct.iter_unpack('<HH', pool[4:]):
        strings.append(data[offset:offset + length].decode('cp1254'))
        offset += length
    if offset != len(data):
        raise ValueError('Corrupt MSI string pool')

    # ponytail: fixed schemas for this release; use msitools for arbitrary MSIs.
    def table(name, widths):
        raw = stream(name)
        if len(raw) % sum(widths):
            raise ValueError('Corrupt MSI table: ' + name)
        count, start, columns = len(raw) // sum(widths), 0, []
        for width in widths:
            columns.append([int.from_bytes(raw[start + i * width:start + (i + 1) * width], 'little') for i in range(count)])
            start += count * width
        return zip(*columns)

    directories = {strings[a]: (strings[b], strings[c]) for a, b, c in table('Directory', [2, 2, 2])}
    components = {strings[r[0]]: strings[r[2]] for r in table('Component', [2] * 6)}

    def directory(key, ancestors=()):
        if key == 'INSTALLDIR':
            return Path('.')
        if key in ancestors:
            raise ValueError('Cyclic MSI directory')
        parent, name = directories[key]
        if not parent:
            return None
        base = directory(parent, ancestors + (key,))
        if base is None:
            return None
        return base / name.split(':')[0].split('|')[-1]

    paths, destinations = {}, set()
    for r in table('File', [2, 2, 2, 4, 2, 2, 2, 2]):
        key, component, name = (strings[r[i]] for i in range(3))
        base = directory(components[component])
        if base is None:
            # Keep only app-local VC7 runtimes, never overwrite Wine system DLLs.
            if name not in ('MFC71.DLL', 'MSVCR71.dll', 'ir41_32.ax'):
                continue
            base = Path('.') if name != 'ir41_32.ax' else Path('legacy-codecs')
        path = base / name.split('|')[-1]
        if path.is_absolute() or '..' in path.parts or '\\' in str(path):
            raise ValueError('Unsafe MSI path: ' + str(path))
        if path in destinations:
            raise ValueError('Duplicate MSI path: ' + str(path))
        destinations.add(path)
        paths[key] = (path, r[3] - 0x80000000)
    return paths


def main():
    target = ROOT / 'game'
    if target.exists():
        raise SystemExit('game/ already exists; refusing to overwrite game settings or saves')
    paths = payload_paths()
    with tempfile.TemporaryDirectory(prefix='.yami-extract-', dir=ROOT) as tmp:
        tmp = Path(tmp)
        raw, staged = tmp / 'cab', tmp / 'game'
        subprocess.run(['cabextract', '-q', '-d', str(raw), str(ROOT / 'Data1.cab')], check=True)
        for key, (path, size) in paths.items():
            source, dest = raw / key, staged / path
            if source.stat().st_size != size:
                raise ValueError('CAB/MSI size mismatch: ' + key)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(source, dest)
        expected = subprocess.check_output(['cabextract', '-p', '-q', '-F', 'eti.exe', str(ROOT / 'Data1.cab')])
        if hashlib.sha256((staged / 'eti.exe').read_bytes()).digest() != hashlib.sha256(expected).digest():
            raise ValueError('Extracted executable does not match original CAB')
        staged.rename(target)
    print(f'Extracted {len(paths)} files into {target}; no installer executed.')


if __name__ == '__main__':
    main()
