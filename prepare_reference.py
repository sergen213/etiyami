#!/usr/bin/env python3
"""Build a windowed reference executable without modifying the original."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ORIGINAL_SHA256 = '361335a4241ba1165f700d9cb61accdec313710becf0a291dec1d1f9300f119e'
# This release's .text VA 0x401000 maps to file offset 0x1000.
PATCHES = (
    (0x4327eb - 0x400000, bytes.fromhex('3b f1 b8 20 00'), bytes.fromhex('e9 2f 00 00 00')),
    (0x432821 - 0x400000, bytes.fromhex('6a 01'), bytes.fromhex('6a 00')),
    (0x4329dc - 0x400000, bytes.fromhex('46'), bytes.fromhex('90')),
    # Borderless window: the original decorated path ignores AdjustWindowRectEx.
    (0x433110 - 0x400000, bytes.fromhex('00 01 04 00'), bytes.fromhex('00 00 04 00')),
    (0x433118 - 0x400000, bytes.fromhex('00 00 cf 00'), bytes.fromhex('00 00 00 80')),
    # WM_ACTIVATE: both WA_ACTIVE (1) and WA_CLICKACTIVE (2) must reacquire input.
    (0x432ff9 - 0x400000, bytes.fromhex('66 3b c1 75 1a'), bytes.fromhex('66 85 c0 74 1a')),
)


def patch(original: bytes) -> bytes:
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Unknown executable; refusing to apply release-specific patches')
    result = bytearray(original)
    for offset, expected, replacement in PATCHES:
        if result[offset:offset + len(expected)] != expected:
            raise ValueError(f'Unexpected instruction at file offset {offset:#x}')
        result[offset:offset + len(expected)] = replacement
    return bytes(result)


if __name__ == '__main__':
    original = (ROOT / 'game/eti.exe').read_bytes()
    target = ROOT / 'game/eti-reference.exe'
    target.write_bytes(patch(original))
    print(f'Created {target}; original eti.exe preserved.')
