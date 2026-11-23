#!/usr/bin/env python3
"""Embed a binary file as a C++ byte array for the PS5 controller."""

from pathlib import Path
import re
import sys


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(
            f"usage: {sys.argv[0]} INPUT OUTPUT.cpp [SYMBOL]",
            file=sys.stderr,
        )
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    symbol = sys.argv[3] if len(sys.argv) == 4 else "prefbar_shellui_elf"
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol):
        print(f"invalid C++ symbol: {symbol}", file=sys.stderr)
        return 2

    data = src.read_bytes()

    dst.parent.mkdir(parents=True, exist_ok=True)
    with dst.open("w", encoding="utf-8", newline="\n") as out:
        out.write("#include <cstddef>\n#include <cstdint>\n\n")
        out.write(f"extern const std::uint8_t {symbol}[] = {{\n")
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset + 16]
            out.write("    ")
            out.write(", ".join(f"0x{b:02x}" for b in chunk))
            out.write(",\n")
        out.write("};\n\n")
        out.write(
            f"extern const std::size_t {symbol}_size = {len(data)}u;\n"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
