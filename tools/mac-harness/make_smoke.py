#!/usr/bin/env python3
"""Emit a tiny PS-X EXE without a MIPS cross compiler or copyrighted BIOS."""
import pathlib
import struct
import sys

BASE = 0x80010000
words = []

def li(reg, value):
    words.extend([0x3C000000 | reg << 16 | value >> 16,
                  0x34000000 | reg << 21 | reg << 16 | (value & 0xFFFF)])

def store(value, offset):
    li(9, value)
    words.append(0xAD090000 | offset)  # sw t1, offset(t0)

li(8, 0xBF801810)
for cmd in [0x00000000, 0x03000000, 0x08000001, 0x05000000,
            0x06000000 | (0xC60 << 12) | 0x260,
            0x07000000 | (256 << 10) | 16]:
    store(cmd, 4)  # GP1 setup, enabled 320x240 display
for cmd in [0xE1000000, 0xE3000000, 0xE4000000 | (239 << 10) | 319,
            0xE5000000, 0xE6000000,
            0x02402010, 0, (240 << 16) | 320,
            0x2000FFFF, (40 << 16) | 160,
            (200 << 16) | 40, (200 << 16) | 280]:
    store(cmd, 0)  # GP0 clear + yellow flat triangle
li(8, 0x8000F000)
store(0x534D4F4B, 0)  # "SMOK" written by emulated CPU
loop = BASE + len(words) * 4
words.extend([0x08000000 | ((loop >> 2) & 0x03FFFFFF), 0])
payload = struct.pack(f"<{len(words)}I", *words)
payload = payload.ljust((len(payload) + 0x7FF) & ~0x7FF, b"\0")
header = bytearray(0x800)
header[:8] = b"PS-X EXE"
struct.pack_into("<IIII", header, 0x10, BASE, 0, BASE, len(payload))
struct.pack_into("<I", header, 0x30, 0x801FFF00)
dest = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "smoke.exe")
dest.parent.mkdir(parents=True, exist_ok=True)
dest.write_bytes(header + payload)
print(f"{dest}: {len(words)} MIPS instructions, {len(payload)} payload bytes")
