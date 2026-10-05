#!/usr/bin/env python3
"""Run a generated System12 GP0 framebuffer/VRAM-coherency probe under Vita3K."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TITLE_ID = "S12FB0001"
EXPECTED = re.compile(
    r"pcsx-vita: system12-framebuffer PASS cpu=ari64 magic=53314642 "
    r"page0=001f page1=03e0 page0again=001f clip=7c00 upload=7fff "
    r"postupload=03e0 move=7c1f source=7c1f runs=1\b")
RESULT_MAGIC = 0x53314642  # S1FB
EXPECTED_PIXELS = (0x001f, 0x03e0, 0x001f, 0x7c00,
                   0x7fff, 0x03e0, 0x7c1f, 0x7c1f)
DISPLAY_W, DISPLAY_H = 320, 240


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import fixture helper: {path}")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def make_guest_code():
    """Generate an ari64 MIPS guest that exercises main-target/VRAM ordering."""
    drc = module("system12_drc_fixture", ROOT / "tests/check_system12_drc.py")
    code = []
    commands = []

    def li(reg, value):
        code.extend((drc.lui(reg, (value >> 16) & 0xffff),
                     drc.ori(reg, reg, value & 0xffff)))

    # GP0 data is at 0xbf801810; GP1 status/control is the adjacent word.
    li(8, 0xbf801810)
    # Keep guest samples independent of register state left by reset.
    li(16, 0x8023f100)

    def gp0(word):
        commands.append(word & 0xffffffff)
        li(9, word)
        code.append(drc.sw(9, 0, 8))

    def gp1(word):
        li(9, word)
        code.append(drc.sw(9, 4, 8))

    def draw_env(x1, y1, x2, y2, offset_x=0, offset_y=0):
        # Inclusive GP0 E3/E4 corners. All framebuffer-sized cases stay >=75%
        # of the 320x240 display in both axes, forcing Native GPU onscreen mode.
        gp0(0xe3000000 | x1 | (y1 << 10))
        gp0(0xe4000000 | x2 | (y2 << 10))
        gp0(0xe5000000 | (offset_x & 0x7ff) | ((offset_y & 0x7ff) << 11))

    def tile8(x, y, pixel):
        # GP0 flat TILE_8: byte color is R,G,B in bits 0..23; RGB555 below
        # uses saturated channels for exact independent readback values.
        r = 255 if pixel & 0x001f else 0
        g = 255 if pixel & 0x03e0 else 0
        b = 255 if pixel & 0x7c00 else 0
        gp0(0x70000000 | r | (g << 8) | (b << 16))
        gp0(x | (y << 16))

    def c0_pixel(x, y, result_index):
        gp0(0xc0000000)
        gp0(x | (y << 16))
        gp0(0x00010001)
        code.extend((drc.lw(10, 0, 8), drc.nop(),
                     (0x0c << 26) | (10 << 21) | (10 << 16) | 0xffff,
                     drc.sw(10, result_index * 4, 16)))

    def upload_pixel(x, y, pixel):
        gp0(0xa0000000)
        gp0(x | (y << 16))
        gp0(0x00010001)
        # Image transfers consume words even for a one-pixel rectangle.
        gp0(pixel | (pixel << 16))

    def move_image(sx, sy, dx, dy, width, height):
        gp0(0x80000000)
        gp0(sx | (sy << 16))
        gp0(dx | (dy << 16))
        gp0(width | (height << 16))

    code.append(drc.sw(0, 4, 8))  # GP1 reset
    gp1(0x05000000)               # display start at VRAM (0,0)
    gp1(0x08000001)               # 320-wide, 240-line display mode
    gp1(0x03000000)               # display enabled

    # (a) Draw page 0 and C0-read before the frame reaches vblank.
    draw_env(0, 0, 319, 239)
    tile8(16, 16, 0x001f)
    c0_pixel(16, 16, 1)

    # (b) Draw the opposite 240-line page using the matching E5 offset.
    draw_env(0, 240, 319, 479, offset_y=240)
    tile8(16, 16, 0x03e0)
    c0_pixel(16, 256, 2)
    c0_pixel(16, 16, 3)

    # (c) Change to a 320x220 clip at y=10 (still onscreen by the 3/4 rule).
    draw_env(0, 10, 319, 229)
    tile8(50, 30, 0x7c00)
    c0_pixel(50, 30, 4)

    # (d) Keep the main target live across a partial A0 upload, then draw
    # outside that write and read both pixels back in command order.
    draw_env(0, 0, 319, 239)
    tile8(40, 40, 0x001f)
    upload_pixel(40, 40, 0x7fff)
    tile8(64, 64, 0x03e0)
    c0_pixel(40, 40, 5)
    c0_pixel(64, 64, 6)

    # (e) MoveImage must see the prior uncommitted draw; destination and
    # source reads confirm both sides of the VRAM copy.
    tile8(80, 80, 0x7c1f)
    move_image(80, 80, 120, 120, 8, 8)
    c0_pixel(120, 120, 7)
    c0_pixel(80, 80, 8)

    li(9, RESULT_MAGIC)
    code.append(drc.sw(9, 0, 16))
    loop = 0xbfc01000 + len(code) * 4
    code.extend((drc.j(loop), drc.nop()))
    return code, commands


def make_regions(path):
    """Reuse the DRC's local System12 board/media regions; replace guest ROM."""
    drc = module("system12_drc_fixture", ROOT / "tests/check_system12_drc.py")
    drc.make_regions(path, 4, vita_signature=True)
    code, _ = make_guest_code()
    if len(code) > (0x400000 - 0x1000) // 4:
        raise RuntimeError("generated framebuffer guest exceeds the main ROM region")
    main = bytearray(0x400000)
    struct.pack_into("<IIII", main, 0, drc.lui(8, 0xbfc0),
                     drc.ori(8, 8, 0x1000), drc.jr(8), drc.nop())
    struct.pack_into(f"<{len(code)}I", main, 0x1000, *code)
    (path / "main.bin").write_bytes(main)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True,
                        help="ARCADE_SYSTEM12=1 Vita VPK with the System12 GPU backend")
    parser.add_argument("--firmware-fs", type=Path, required=True)
    parser.add_argument("--shader-compiler", type=Path, required=True,
                        help="existing local libshacccg.suprx")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if (args.output_dir.exists() or not args.emulator.exists() or
            not args.vpk.is_file() or not args.firmware_fs.is_dir() or
            not args.shader_compiler.is_file() or
            not 0 < args.timeout < float("inf")):
        parser.error("require an existing emulator/VPK/firmware/shader compiler, fresh output directory, and positive timeout")

    checker = module("vram_checker", Path(__file__).with_name("check_vita3k.py"))
    drc = module("vram_host_helpers", Path(__file__).with_name("check_system12_drc_vita3k.py"))
    gameplay = module("vram_pack_helpers", Path(__file__).with_name("check_system12_gameplay_vita3k.py"))
    checker.CapturedSmoke.ISSUE_MARKERS += (b"pcsx-vita: system12-framebuffer FAIL",)
    args.output_dir.mkdir(parents=True)
    log = args.output_dir / "vita3k.log"
    passed = False
    with tempfile.TemporaryDirectory(prefix="pcsx-vita-framebuffer-") as directory:
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
        region = temp / "framebuffer"
        region.mkdir()
        make_regions(region)
        (args.output_dir / "board.cfg").write_bytes((region / "board.cfg").read_bytes())
        packer = Path(shutil.which("vita-pack-vpk")) if shutil.which("vita-pack-vpk") else None
        mksfoex = Path(shutil.which("vita-mksfoex")) if shutil.which("vita-mksfoex") else None
        if not packer or not mksfoex:
            parser.error("vita-pack-vpk and vita-mksfoex are required")
        vpk = gameplay.make_vpk(args.vpk, packer, mksfoex,
            temp / "system12-framebuffer.vpk", temp, TITLE_ID,
            "PCSX System12 framebuffer fixture",
            [(p, "framebuffer/" + p.name) for p in region.iterdir()])
        base = [str(emulator), "--keep-config", "--log-level", "1"]
        installed = checker.run_owned(base + ["--", str(vpk)], log, args.timeout,
            "framebuffer fixture install", f"[{TITLE_ID}] installed successfully!", installation=True)
        passed = installed and checker.run_owned(base + ["--installed-path", TITLE_ID,
            "--app-args", f"pcsx-vita, --system12, ux0:app/{TITLE_ID}/framebuffer, "
            "--verify-system12-framebuffer, --frames, 1"], log, args.timeout,
            "System12 framebuffer coherence", EXPECTED, require_ari64=True)
        evidence = dict(passed=passed, expected=EXPECTED.pattern,
            colors_rgb555=list(EXPECTED_PIXELS),
            main_sha256=gameplay.sha256_file(region / "main.bin"),
            host_sha256=gameplay.sha256_file(emulator),
            vpk_sha256=gameplay.sha256_file(args.vpk),
            shader_compiler_sha256=gameplay.sha256_file(args.shader_compiler),
            log=str(log.resolve()))
        (args.output_dir / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Vita3K System12 framebuffer coherence: {'PASS' if passed else 'FAIL'}; log={log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
