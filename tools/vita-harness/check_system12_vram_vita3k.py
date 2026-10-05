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
PATTERN_MAGIC = 0x53315458  # "S1TX", appended ABI/version marker for texture probe v1.
P8_INDICES = (0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef)
P8_INDICES_REFRESH = (0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe)
TEXTURE_SAMPLE_COUNT = 48


def p4_palette(refresh):
    if not refresh:
        return tuple(((i * 7 + 1) & 31) | (((i * 11 + 2) & 31) << 5) |
                     (((i * 13 + 3) & 31) << 10) for i in range(16))
    return tuple(((i * 9 + 4) & 31) | (((i * 3 + 7) & 31) << 5) |
                 (((i * 15 + 8) & 31) << 10) for i in range(16))


def p8_palette(refresh):
    if not refresh:
        return tuple(((i * 3 + 1) & 31) | (((i * 5 + 2) & 31) << 5) |
                     (((i * 7 + 3) & 31) << 10) for i in range(256))
    return tuple(((i * 11 + 9) & 31) | (((i * 13 + 5) & 31) << 5) |
                 (((i * 17 + 1) & 31) << 10) for i in range(256))


def expected_texture_values():
    """Independent pixel/CLUT oracle for the two upload-and-refresh passes."""
    return (p4_palette(False) + tuple(p8_palette(False)[i] for i in P8_INDICES) +
            p4_palette(True) + tuple(p8_palette(True)[i] for i in P8_INDICES_REFRESH))


def texture_signature(values=None):
    values = expected_texture_values() if values is None else values
    value = 2166136261
    for pixel in values:
        for byte in (pixel & 0xff, (pixel >> 8) & 0xff):
            value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def pack_p4_texture(pixels):
    assert len(pixels) == 16 * 8
    return [pixels[i] | (pixels[i + 1] << 4) | (pixels[i + 2] << 8) |
            (pixels[i + 3] << 12) for i in range(0, len(pixels), 4)]


def pack_p8_texture(pixels):
    assert len(pixels) == 16 * 8
    return [pixels[i] | (pixels[i + 1] << 8) for i in range(0, len(pixels), 2)]


def verify_texture_encoding(p4_pixels, p4_words, p8_pixels, p8_words):
    """Reconstruct the GPU-facing VRAM lanes from the encoded upload words."""
    decoded_p4 = [((word >> shift) & 15) for word in p4_words
                  for shift in (0, 4, 8, 12)]
    decoded_p8 = [((word >> shift) & 0xff) for word in p8_words
                  for shift in (0, 8)]
    assert decoded_p4 == p4_pixels
    assert decoded_p8 == p8_pixels


def texture_word_location(tpage, u, v, depth):
    page_x = (tpage & 15) * 64
    page_y = ((tpage >> 4) & 1) * 256 + ((tpage >> 11) & 1) * 512
    pixels_per_word = {4: 4, 8: 2, 16: 1}[depth]
    return page_x + u // pixels_per_word, page_y + v


PATTERN_SIGNATURE = texture_signature()
EXPECTED_PATTERN = re.compile(
    EXPECTED.pattern[:-2] +
    rf" texture=PASS sig={PATTERN_SIGNATURE:08x} samples={TEXTURE_SAMPLE_COUNT}\b")


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def make_regions(path, texture_pattern=False):
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
    def upload_rect(x, y, width, height, pixels):
        assert len(pixels) == width * height and len(pixels) % 2 == 0
        gp0(0xa0000000)
        gp0(x | (y << 16))
        gp0(width | (height << 16))
        for i in range(0, len(pixels), 2):
            gp0(pixels[i] | (pixels[i + 1] << 16))
    def draw_and_read(tpage, clut_x, uv_x, uv_y, screen_x, screen_y, index):
        clut = (800 << 6) | (clut_x >> 4)
        assert (clut >> 6) == 800 and (clut & 63) * 16 == clut_x
        for word in (0xe1000000 | tpage, 0x64808080,
                     screen_x | (screen_y << 16),
                     (uv_x | (uv_y << 8)) | (clut << 16), 0x00010001):
            gp0(word)
        read_pixel(screen_x, screen_y, index)
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
    if texture_pattern:
        p4_passes = (
            lambda x, y: (x + y * 3) & 15,
            lambda x, y: (15 - x + y * 5) & 15,
        )
        p8_passes = (P8_INDICES, P8_INDICES_REFRESH)
        output = 13
        for refresh in range(2):
            p4_pixels = [p4_passes[refresh](x, y) for y in range(8) for x in range(16)]
            p8_pixels = [p8_passes[refresh][(x + y * 3) & 7]
                         for y in range(8) for x in range(16)]
            p4_words = pack_p4_texture(p4_pixels)
            p8_words = pack_p8_texture(p8_pixels)
            verify_texture_encoding(p4_pixels, p4_words, p8_pixels, p8_words)
            upload_rect(0, 512, 4, 8, p4_words)
            upload_rect(32, 800, 16, 1, p4_palette(bool(refresh)))
            upload_rect(128, 520, 8, 8, p8_words)
            upload_rect(128, 800, 256, 1, p8_palette(bool(refresh)))

            for palette_index in range(16):
                uv_y = palette_index & 7
                p4_row = p4_pixels[uv_y * 16:(uv_y + 1) * 16]
                uv_x = p4_row.index(palette_index)
                assert texture_word_location(0x0800, uv_x, uv_y, 4) == (
                    uv_x // 4, 512 + uv_y)
                draw_and_read(0x0800, 32, uv_x, uv_y, 96 + palette_index,
                              610 + refresh, output)
                output += 1
            for uv_y, palette_index in enumerate(p8_passes[refresh]):
                p8_row = p8_pixels[uv_y * 16:(uv_y + 1) * 16]
                uv_x = p8_row.index(palette_index)
                assert texture_word_location(0x0882, uv_x, 8 + uv_y, 8) == (
                    128 + uv_x // 2, 520 + uv_y)
                draw_and_read(0x0882, 128, uv_x, 8 + uv_y, 96 + (output % 16),
                              612 + refresh, output)
                output += 1
        assert output == 13 + TEXTURE_SAMPLE_COUNT
        li(9, PATTERN_MAGIC)
        code.append(fixture.sw(9, 12 * 4, 16))  # Extended marker follows all texture reads.
    li(9, 0x53313256)
    code.append(fixture.sw(9, 0, 16))  # Base completion marker follows all GPU reads.
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
    parser.add_argument("--texture-pattern", action="store_true",
                        help="also probe varied upper-VRAM P4/P8 textures and refreshed CLUTs")
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
        make_regions(region, texture_pattern=args.texture_pattern)
        (args.output_dir / "board.cfg").write_bytes((region / "board.cfg").read_bytes())
        vpk = gameplay.make_vpk(args.vpk, Path(shutil.which("vita-pack-vpk")),
            Path(shutil.which("vita-mksfoex")), temp / "vram.vpk", temp, TITLE_ID,
            "PCSX System12 upper VRAM", [(p, "vram/" + p.name) for p in region.iterdir()])
        base = [str(emulator), "--keep-config", "--log-level", "1"]
        installed = checker.run_owned(base + ["--", str(vpk)], log, args.timeout,
            "VRAM fixture install", f"[{TITLE_ID}] installed successfully!", installation=True)
        verify_arg = "--verify-system12-vram-pattern" if args.texture_pattern else "--verify-system12-vram"
        expected = EXPECTED_PATTERN if args.texture_pattern else EXPECTED
        passed = installed and checker.run_owned(base + ["--installed-path", TITLE_ID,
            "--app-args", f"pcsx-vita, --system12, ux0:app/{TITLE_ID}/vram, "
            f"{verify_arg}, --frames, 1"], log, args.timeout,
            "System12 upper VRAM", expected, require_ari64=True)
        evidence = dict(passed=passed, vpk_sha256=gameplay.sha256_file(args.vpk),
            host_sha256=gameplay.sha256_file(emulator),
            shader_compiler_sha256=gameplay.sha256_file(args.shader_compiler),
            main_sha256=gameplay.sha256_file(region / "main.bin"),
            expected=expected.pattern, log=str(log.resolve()))
        (args.output_dir / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"Vita3K System12 upper VRAM: {'PASS' if passed else 'FAIL'}; log={log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
