#!/usr/bin/env python3
"""Run a generated Type-2 upper-VRAM MMIO guest under a guarded Vita3K host."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TITLE_ID = "S12VRAM01"
EXPECTED = re.compile(
    r"pcsx-vita: system12-vram PASS cpu=ari64 magic=53313256 "
    r"p4=7c00 p8=7c00 odd8=7c00 p16=03e0 lower=0002 last=1234 wrap=5678 lower4=001f upper4=7c00 filllast=03e0 fillwrap=03e0 runs=1\b")


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def make_regions(path):
    """Mirror the local MAME GPU oracle through actual guest GP0 stores/loads."""
    import struct
    fixture = module("vram_drc_fixture", ROOT / "tests/check_system12_drc.py")
    fixture.make_regions(path, 4, vita_signature=True)
    code = []
    def li(reg, value):
        code.extend((fixture.lui(reg, value >> 16), fixture.ori(reg, reg, value)))
    li(8, 0xbf801810)
    li(16, 0x8023f100)
    def gp0(word):
        li(9, word)
        code.append(fixture.sw(9, 0, 8))
    def upload(x, y, pixel):
        for word in (0xa0000000, x | y << 16, 0x00010001, pixel | pixel << 16):
            gp0(word)
    def read_pixel(x, y, index):
        for word in (0xc0000000, x | y << 16, 0x00010001):
            gp0(word)
        code.extend((fixture.lw(10, 0, 8), fixture.nop(),
                     (0x0c << 26) | 10 << 21 | 10 << 16 | 0xffff,
                     fixture.sw(10, index * 4, 16)))
    code.append(fixture.sw(0, 4, 8))  # GP1 reset
    # Small offscreen draw window avoids a 1024x1024 main RGBA render target.
    for word in (0xe3000000 | 96 | 592 << 10,
                 0xe4000000 | 127 | 623 << 10, 0xe5000000, 0xe6000000):
        gp0(word)
    upload(0, 512, 1)
    upload(1, 800, 0x7c00)
    def sprite(tpage, x, index):
        for word in (0xe1000000 | tpage, 0x64808080, x | 600 << 16,
                     (800 * 64) << 16, 0x00010001):
            gp0(word)
        read_pixel(x, 600, index)
    sprite(0x0800, 100, 1)
    upload(0, 0, 2)
    upload(2, 800, 0x001f)
    sprite(0x0000, 104, 8)
    sprite(0x0800, 105, 9)
    sprite(0x0880, 101, 2)
    upload(64, 512, 1)
    sprite(0x0881, 102, 3)
    upload(0, 512, 0x03e0)
    sprite(0x0900, 103, 4)
    read_pixel(0, 0, 5)  # Upper VRAM must preserve the distinct lower-bank payload.
    for word in (0xa0000000, 10 | 1023 << 16, 0x00020001, 0x56781234):
        gp0(word)
    read_pixel(10, 1023, 6)
    read_pixel(10, 0, 7)
    for word in (0x0200ff00, 32 | 1023 << 16, 16 | 2 << 16):
        gp0(word)
    read_pixel(32, 1023, 10)
    read_pixel(32, 0, 11)
    li(9, 0x53313256)
    code.append(fixture.sw(9, 0, 16))  # Completion marker follows all GPU reads.
    loop = 0xbfc01000 + len(code) * 4
    code.extend((fixture.j(loop), fixture.nop()))
    main = bytearray(0x400000)
    struct.pack_into("<IIII", main, 0, fixture.lui(8, 0xbfc0),
                     fixture.ori(8, 8, 0x1000), fixture.jr(8), fixture.nop())
    struct.pack_into(f"<{len(code)}I", main, 0x1000, *code)
    (path / "main.bin").write_bytes(main)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True, help="ARCADE_SYSTEM12=1 VPK")
    parser.add_argument("--firmware-fs", type=Path, required=True)
    parser.add_argument("--shader-compiler", type=Path, required=True, help="existing local libshacccg.suprx")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.output_dir.exists() or not 0 < args.timeout < float("inf"):
        parser.error("use a fresh output directory and positive bounded timeout")
    checker = module("vram_checker", Path(__file__).with_name("check_vita3k.py"))
    drc = module("vram_host_helpers", Path(__file__).with_name("check_system12_drc_vita3k.py"))
    gameplay = module("vram_pack_helpers", Path(__file__).with_name("check_system12_gameplay_vita3k.py"))
    checker.CapturedSmoke.ISSUE_MARKERS += (b"pcsx-vita: system12-vram FAIL",)
    args.output_dir.mkdir(parents=True)
    log = args.output_dir / "vita3k.log"
    with tempfile.TemporaryDirectory(prefix="pcsx-vita-vram-") as directory:
        temp = Path(directory)
        bundle, binary = drc.locate_app_bundle(args.emulator)
        isolated = temp / bundle.name
        shutil.copytree(bundle, isolated, symlinks=True)
        emulator = isolated / binary
        portable = temp / "portable"
        portable.mkdir()
        drc.make_isolated_config(portable / "config.yml", portable / "fs",
            args.firmware_fs, args.firmware_fs.parent / "config.yml")
        gameplay.set_png_screenshot_format(portable / "config.yml")
        checker.install_local_shader_compiler(emulator, args.shader_compiler)
        region = temp / "vram"
        region.mkdir()
        make_regions(region)
        (args.output_dir / "board.cfg").write_bytes((region / "board.cfg").read_bytes())
        vpk = gameplay.make_vpk(args.vpk, Path(shutil.which("vita-pack-vpk")),
            Path(shutil.which("vita-mksfoex")), temp / "vram.vpk", temp, TITLE_ID,
            "PCSX System12 upper VRAM", [(p, "vram/" + p.name) for p in region.iterdir()])
        base = [str(emulator), "--keep-config", "--log-level", "1"]
        installed = checker.run_owned(base + ["--", str(vpk)], log, args.timeout,
            "VRAM fixture install", f"[{TITLE_ID}] installed successfully!", installation=True)
        passed = installed and checker.run_owned(base + ["--installed-path", TITLE_ID,
            "--app-args", f"pcsx-vita, --system12, ux0:app/{TITLE_ID}/vram, "
            "--verify-system12-vram, --frames, 1"], log, args.timeout,
            "System12 upper VRAM", EXPECTED, require_ari64=True)
        evidence = dict(passed=passed, vpk_sha256=gameplay.sha256_file(args.vpk),
            host_sha256=gameplay.sha256_file(emulator),
            shader_compiler_sha256=gameplay.sha256_file(args.shader_compiler),
            main_sha256=gameplay.sha256_file(region / "main.bin"),
            expected=EXPECTED.pattern, log=str(log.resolve()))
        (args.output_dir / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Vita3K System12 upper VRAM: {'PASS' if passed else 'FAIL'}; log={log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
