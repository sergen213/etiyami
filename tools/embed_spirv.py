#!/usr/bin/env python3
"""Embed compiled little-endian SPIR-V in a standalone C++ header."""

import argparse
import os
from pathlib import Path
import re
import struct
import tempfile


CPP_KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break
case catch char char8_t char16_t char32_t class compl concept const consteval
constexpr constinit const_cast continue co_await co_return co_yield decltype
default delete do double dynamic_cast else enum explicit export extern false
float for friend goto if inline int long mutable namespace new noexcept not
not_eq nullptr operator or or_eq private protected public register
reinterpret_cast requires return short signed sizeof static static_assert
static_cast struct switch template this thread_local throw true try typedef
typeid typename union unsigned using virtual void volatile wchar_t while xor
xor_eq""".split())


def symbol_for(path: Path) -> str:
    name = path.name.removesuffix(".spv")
    name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not name or name[0].isdigit() or name in CPP_KEYWORDS or name.startswith("_"):
        name = "shader_" + name
    # Double underscores are reserved by C++, even inside an identifier.
    return re.sub(r"_+", "_", name)


def embed(output: Path, inputs: list[Path]) -> None:
    arrays = {}
    for path in inputs:
        name = symbol_for(path)
        if name in arrays:
            raise ValueError(f"{path}: duplicate shader symbol '{name}'")
        data = path.read_bytes()
        if len(data) < 20 or len(data) % 4:
            raise ValueError(f"{path}: SPIR-V size must be at least 20 bytes and divisible by 4")
        words = struct.unpack(f"<{len(data) // 4}I", data)
        if words[0] != 0x07230203:
            raise ValueError(f"{path}: invalid little-endian SPIR-V magic")
        arrays[name] = words

    lines = ["#pragma once", "#include <cstdint>", "", "namespace yami::spirv {"]
    for name, words in sorted(arrays.items()):
        lines.append(f"inline constexpr std::uint32_t {name}[] = {{")
        for start in range(0, len(words), 8):
            lines.append("    " + ", ".join(f"0x{word:08x}u" for word in words[start:start + 8]) + ",")
        lines.append("};")
    lines.append("} // namespace yami::spirv")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", newline="\n",
                                         dir=output.parent, prefix=output.name + ".",
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write("\n".join(lines) + "\n")
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def self_test() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        shader = root / "effects_rt.frag.spv"
        words = (0x07230203, 0x00010600, 0, 1, 0)
        shader.write_bytes(struct.pack("<5I", *words))
        output = root / "headers" / "shaders.hpp"
        embed(output, [shader])
        expected = ("#pragma once\n#include <cstdint>\n\nnamespace yami::spirv {\n"
                    "inline constexpr std::uint32_t effects_rt_frag[] = {\n"
                    "    0x07230203u, 0x00010600u, 0x00000000u, 0x00000001u, 0x00000000u,\n"
                    "};\n} // namespace yami::spirv\n").encode("ascii")
        assert output.read_bytes() == expected
        collision = root / "effects_rt-frag.spv"
        collision.write_bytes(shader.read_bytes())
        bad = root / "bad.spv"
        try:
            embed(output, [shader, collision])
        except ValueError as error:
            assert "duplicate shader symbol" in str(error)
        else:
            raise AssertionError("collision accepted")
        for data in (b"", b"\0" * 19, b"\0" * 21, b"\0" * 20,
                     struct.pack(">5I", *words)):
            bad.write_bytes(data)
            try:
                embed(output, [bad])
            except ValueError:
                pass
            else:
                raise AssertionError("malformed SPIR-V accepted")
            assert output.read_bytes() == expected
        assert symbol_for(Path("9-test.vert.spv")) == "shader_9_test_vert"
    print("SPIR-V embedding self-test passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("inputs", nargs="*", type=Path)
    args = parser.parse_args()
    if args.self_test:
        if args.output or args.inputs:
            parser.error("--self-test cannot be combined with --output or inputs")
        self_test()
        return
    if args.output is None or not args.inputs:
        parser.error("--output and at least one compiled SPIR-V input are required")
    try:
        embed(args.output, args.inputs)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
