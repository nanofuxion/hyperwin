#!/usr/bin/env python3
"""Embed iasl-produced AML tables in a freestanding C header."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("name_path", nargs="+", help="C identifier=AML path")
    args = parser.parse_args()

    lines = ["/* Generated from the ASL sources by iasl. */", "#pragma once", "#include <stdint.h>"]
    for item in args.name_path:
        name, path = item.split("=", 1)
        data = Path(path).read_bytes()
        if len(data) < 36:
            raise SystemExit(f"{path}: AML table is shorter than an ACPI header")
        lines.append(f"static const uint8_t {name}[] = {{")
        for start in range(0, len(data), 12):
            lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[start : start + 12]) + ",")
        lines.append("};")
        lines.append(f"#define {name.upper()}_SIZE {len(data)}u")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
