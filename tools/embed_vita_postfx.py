#!/usr/bin/env python3
"""Embed Vita Cg shader binaries into eboot for both installed and host0 launches."""

import argparse
from pathlib import Path


def array(name: str, payload: bytes) -> str:
    lines = [f"static const unsigned char {name}[] __attribute__((aligned(4))) = {{"]
    for offset in range(0, len(payload), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in payload[offset:offset + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("effects", nargs="+")
    args = parser.parse_args()

    parts = ['#include "postfx_binaries.h"']
    rows = []
    for effect in args.effects:
        for stage in ("vert", "frag"):
            binary_path = args.source / effect / f"{stage}.gxp"
            data = binary_path.read_bytes()
            if len(data) < 16:
                raise RuntimeError(f"Invalid shader: {binary_path}")
            parts.append(array(f"postfx_{effect}_{stage}", b"\x00\x00\x00\x00" + data))
        rows.append(
            f'    {{"{effect}", postfx_{effect}_vert, sizeof(postfx_{effect}_vert), '
            f'postfx_{effect}_frag, sizeof(postfx_{effect}_frag)}},'
        )
    parts.append("const VitaPostFXBinary vita_postfx_binaries[] = {\n" + "\n".join(rows) + "\n};")
    parts.append("const unsigned vita_postfx_binary_count = "
                 "sizeof(vita_postfx_binaries) / sizeof(vita_postfx_binaries[0]);")
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    output = "\n\n".join(parts) + "\n"
    if not args.destination.exists() or args.destination.read_text(encoding="utf-8") != output:
        args.destination.write_text(output, encoding="utf-8")


if __name__ == "__main__":
    main()
