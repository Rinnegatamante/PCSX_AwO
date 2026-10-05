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
    r"postupload=03e0 move=7c1f source=7c1f "
    r"ft3a_tl=001f ft3a_tr=03e0 ft3a_bl=7c00 "
    r"ft3b_tl=7fe0 ft3b_tr=7c1f ft3b_bl=7fff "
    r"ft4_tl=7fe0 ft4_tr=7c1f ft4_bl=7fff ft4_br=4210 runs=1\b")
EXPECTED_TEXTURE_WINDOW = re.compile(
    r"pcsx-vita: system12-texture-window PASS cpu=ari64 magic=53314642 "
    r"ft3a_tl=001f ft3a_tr=03e0 ft3a_bl=7c00 "
    r"ft3b_tl=7fe0 ft3b_tr=7c1f ft3b_bl=7fff "
    r"ft4_tl=7fe0 ft4_tr=7c1f ft4_bl=7fff ft4_br=4210 "
    r"tw_tl=7c1f tw_tr=7c1f tw_br=4210 "
    r"reset_tl=7fe0 reset_tr=7c1f reset_bl=7fff runs=1\b")
EXPECTED_VRAM_X_WRAP = re.compile(
    r"pcsx-vita: system12-vram-x-wrap PASS cpu=ari64 magic=53314642 "
    r"p8_poly_u128=7fe0 p8_poly_u136=7fe0 "
    r"p8_sprite_u120=7fe0 p8_sprite_u136=7c1f p8_clut_x4=7c1f "
    r"p16_poly_u64=001f p16_poly_u72=03e0 "
    r"p16_sprite_u56=001f p16_sprite_u64=03e0 runs=1\b")
EXPECTED_COORD_ALIAS = re.compile(
    r"pcsx-vita: system12-coord-alias PASS cpu=ari64 magic=53314642 "
    r"ft3_control=001f ft3_upper=03e0 ft3_negative=03e0 "
    r"ft4_control=001f ft4_upper=03e0 ft4_negative=03e0 "
    r"sprite_control=001f sprite_upper=03e0 sprite_negative=03e0 runs=1\b")
EXPECTED_COORD_SPAN_CULL = re.compile(
    r"pcsx-vita: system12-coord-span-cull PASS cpu=ari64 magic=53314642 "
    r"ft3_1024_before=001f ft3_1024_after=001f "
    r"ft3_1023_before=001f ft3_1023_after=03e0 "
    r"f4_invalid_before=001f f4_valid_before=001f "
    r"f4_invalid_after=001f f4_valid_after=03e0 "
    r"ft4_invalid_before=001f ft4_valid_before=001f "
    r"ft4_invalid_after=001f ft4_valid_after=03e0 "
    r"culled_ft3_tpage=03e0 g4_valid_half=03e0 runs=1\b")
RESULT_MAGIC = 0x53314642  # S1FB
DEFAULT_MAIN_SHA256 = "07cbcdecd6a8c4c6cb3b85b060dbd211e06e70f2fea5af62336099c458661bf6"
TEXTURE_WINDOW_MAIN_SHA256 = "8eb3780d65e540b668f82e52899c41721ef416201a9d0e0683ff59b4a85a6562"
DEFAULT_GP0_SHA256 = "66469427106cc45f1798a85949e63af3ad2f56f5e7856be251acfcd49abecf52"
SPRITE_UV_WRAP_MAIN_SHA256 = "bfe0204a0b50d8c24b0f199476d8e9d0fa3c72e72b27f6811938ae224210a6d8"
SPRITE_UV_WRAP_GP0_SHA256 = "b5604049f2cbca0498a9fc53923fa79eec342a530eb872bf2530f07a3bfa631e"
VRAM_X_WRAP_MAIN_SHA256 = "2a1768d5f960371b5560dc6a54209a5e5b7b1e3b9c2d1d0cda306b7d4131bea6"
VRAM_X_WRAP_GP0_SHA256 = "d019567552e3affa46513cc566098454f1cb9b9f21d372707684e2ddfbca2946"
COORD_ALIAS_MAIN_SHA256 = "826e4e5de3ddddc135dc7ee2a4e1fd0d41bf44921b2f88c02c232df9071e6802"
COORD_ALIAS_GP0_SHA256 = "185afe4f88d9594e7deafb7417d4cc2d4400b0b50894eff20a9b816c342f6306"
COORD_SPAN_CULL_MAIN_SHA256 = "d8d133b295a302c5139273b8da75f30f1ed90f9462aee5ffbaa21864da834266"
COORD_SPAN_CULL_GP0_SHA256 = "59c7ad50ba42d8870b044e9dd008da0d2e593b5481732f84c0376e081a608acb"
EXPECTED_COORD_ALIAS_PIXELS = (0x001f, 0x03e0, 0x03e0,
                               0x001f, 0x03e0, 0x03e0,
                               0x001f, 0x03e0, 0x03e0)
EXPECTED_COORD_SPAN_CULL_PIXELS = (0x001f, 0x001f,
                                   0x001f, 0x03e0,
                                   0x001f, 0x001f,
                                   0x001f, 0x03e0,
                                   0x001f, 0x001f,
                                   0x001f, 0x03e0,
                                   0x03e0, 0x03e0)
EXPECTED_VRAM_X_WRAP_PIXELS = (0x7fe0, 0x7fe0, 0x7fe0, 0x7c1f, 0x7c1f,
                               0x001f, 0x03e0, 0x001f, 0x03e0)
EXPECTED_PIXELS = (0x001f, 0x03e0, 0x001f, 0x7c00,
                   0x7fff, 0x03e0, 0x7c1f, 0x7c1f)
EXPECTED_POLYGON_PIXELS = (0x001f, 0x03e0, 0x7c00,
                           0x7fe0, 0x7c1f, 0x7fff,
                           0x7fe0, 0x7c1f, 0x7fff, 0x4210)
EXPECTED_TEXTURE_WINDOW_PIXELS = (0x7c1f, 0x7c1f, 0x4210,
                                  0x7fe0, 0x7c1f, 0x7fff)
EXPECTED_SPRITE_UV_WRAP_PIXELS = (0x03ff, 0x7c00, 0x03e0, 0x001f, 0x7c00,
                                 0x4210, 0x7fff, 0x7c1f, 0x7fe0, 0x7fff,
                                 0x03ff, 0x7c00, 0x03e0, 0x001f, 0x7c00)
DISPLAY_W, DISPLAY_H = 320, 240


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import fixture helper: {path}")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def probe_configuration(texture_window=False, vram_x_wrap=False, coord_alias=False,
                        coord_span_cull=False):
    """Return the Vita verifier, log oracle, and colors for one fixture mode."""
    if sum((texture_window, vram_x_wrap, coord_alias, coord_span_cull)) > 1:
        raise ValueError("texture-window, VRAM-X-wrap, coordinate-alias, and coordinate-span probes are separate modes")
    if texture_window:
        return ("--verify-system12-texture-window", EXPECTED_TEXTURE_WINDOW,
                "System12 texture-window probe", EXPECTED_TEXTURE_WINDOW_PIXELS)
    if vram_x_wrap:
        return ("--verify-system12-vram-x-wrap", EXPECTED_VRAM_X_WRAP,
                "System12 VRAM X-wrap probe", EXPECTED_VRAM_X_WRAP_PIXELS)
    if coord_alias:
        return ("--verify-system12-coord-alias", EXPECTED_COORD_ALIAS,
                "System12 coordinate-alias probe", EXPECTED_COORD_ALIAS_PIXELS)
    if coord_span_cull:
        return ("--verify-system12-coord-span-cull", EXPECTED_COORD_SPAN_CULL,
                "System12 coordinate-span culling probe", EXPECTED_COORD_SPAN_CULL_PIXELS)
    return ("--verify-system12-framebuffer", EXPECTED,
            "System12 framebuffer coherence", EXPECTED_PIXELS)


def make_guest_code(texture_window=False, sprite_uv_wrap=False, vram_x_wrap=False,
                    coord_alias=False, coord_span_cull=False):
    """Generate an ari64 MIPS guest that exercises main-target/VRAM ordering."""
    if sum((texture_window, sprite_uv_wrap, vram_x_wrap, coord_alias,
            coord_span_cull)) > 1:
        raise ValueError("texture-window, sprite-UV-wrap, VRAM-X-wrap, coordinate-alias, and coordinate-span probes are separate modes")
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

    def upload_rect(x, y, width, height, pixels):
        assert len(pixels) == width * height and len(pixels) % 2 == 0
        gp0(0xa0000000)
        gp0(x | (y << 16))
        gp0(width | (height << 16))
        for i in range(0, len(pixels), 2):
            gp0(pixels[i] | (pixels[i + 1] << 16))

    def upload_p4_test_page(word_x, height=8):
        # Four broad index regions make interpolated UV samples unambiguous,
        # with enough margin that edge fill and subpixel rules do not matter.
        texels = []
        for y in range(height):
            for word_x_inner in range(4):
                indices = []
                for lane in range(4):
                    x = word_x_inner * 4 + lane
                    if y < height // 2:
                        indices.append(1 if x < 8 else 2)
                    else:
                        indices.append(3 if x < 8 else 4)
                texels.append(indices[0] | (indices[1] << 4) |
                              (indices[2] << 8) | (indices[3] << 12))
        upload_rect(word_x, 512, 4, height, texels)

    def upload_palette(x, y, entries):
        assert len(entries) == 16
        upload_rect(x, y, 16, 1, entries)

    def move_image(sx, sy, dx, dy, width, height):
        gp0(0x80000000)
        gp0(sx | (sy << 16))
        gp0(dx | (dy << 16))
        gp0(width | (height << 16))

    def ft3(x, y, uv_points, tpage, clut, color=0x808080):
        assert len(uv_points) == 3
        gp0(0x24000000 | color)
        for index, (px, py) in enumerate(((x, y), (x + 64, y), (x, y + 64))):
            gp0(px | (py << 16))
            u, v = uv_points[index]
            word = u | (v << 8)
            if index == 0:
                word |= clut << 16
            elif index == 1:
                word |= tpage << 16
            gp0(word)

    def ft4(x, y, uv_points, tpage, clut, color=0x808080):
        assert len(uv_points) == 4
        gp0(0x2c000000 | color)
        positions = ((x, y), (x + 64, y), (x, y + 64), (x + 64, y + 64))
        for index, ((px, py), (u, v)) in enumerate(zip(positions, uv_points)):
            gp0(px | (py << 16))
            word = u | (v << 8)
            if index == 0:
                word |= clut << 16
            elif index == 1:
                word |= tpage << 16
            gp0(word)

    def ft3_raw(position_words, uv_points, tpage, clut, color=0x808080):
        assert len(position_words) == len(uv_points) == 3
        gp0(0x24000000 | color)
        for index, (position, (u, v)) in enumerate(zip(position_words, uv_points)):
            gp0(position)
            word = u | (v << 8)
            if index == 0:
                word |= clut << 16
            elif index == 1:
                word |= tpage << 16
            gp0(word)

    def ft4_raw(position_words, uv_points, tpage, clut, color=0x808080):
        assert len(position_words) == len(uv_points) == 4
        gp0(0x2c000000 | color)
        for index, (position, (u, v)) in enumerate(zip(position_words, uv_points)):
            gp0(position)
            word = u | (v << 8)
            if index == 0:
                word |= clut << 16
            elif index == 1:
                word |= tpage << 16
            gp0(word)

    def upload_constant_words(x, y, width, height, value):
        upload_rect(x, y, width, height, [value] * (width * height))

    def emit_uv_wrap_sprite(x, y, tpage, clut):
        gp0(0xe1000000 | tpage)
        gp0(0x64000000 | 0x808080)
        gp0(x | (y << 16))
        gp0(252 | (252 << 8) | (clut << 16))
        # Width exceeds one complete U period and therefore exercises both
        # the initial seam and the next wrap after 256 more texels.
        gp0(272 | (16 << 16))

    def emit_variable_sprite(x, y, u, v, width, height, tpage, clut):
        gp0(0xe1000000 | tpage)
        gp0(0x64000000 | 0x808080)
        gp0(x | (y << 16))
        gp0(u | (v << 8) | (clut << 16))
        gp0(width | (height << 16))

    def emit_variable_sprite_raw(xy, u, v, width, height, tpage, clut):
        gp0(0xe1000000 | tpage)
        gp0(0x64000000 | 0x808080)
        gp0(xy)
        gp0(u | (v << 8) | (clut << 16))
        gp0(width | (height << 16))

    def upload_p4_wrap_quadrants():
        # Four broad 16x8 texel patches around both 8-bit coordinate seams.
        for x, y, index in ((0, 0, 1), (60, 0, 2),
                            (0, 248, 3), (60, 248, 4)):
            upload_constant_words(x, y, 4, 8, index * 0x1111)

    def upload_p8_wrap_quadrants():
        for x, y, index in ((0, 0, 1), (124, 0, 2),
                            (0, 248, 3), (124, 248, 4)):
            upload_constant_words(x, y, 4, 8, index * 0x0101)

    def upload_p16_wrap_quadrants():
        for x, y, color in ((0, 0, 0x001f), (248, 0, 0x03e0),
                            (0, 248, 0x7c00), (248, 248, 0x03ff)):
            upload_constant_words(x, y, 8, 8, color)

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

    # Interpolated polygon probe: successive FT3/FT3/FT4 packets share one
    # onscreen clip while changing CLUT and then TPAGE. Constant-index texture
    # quadrants let interior C0 reads act as a stable oracle without modeling
    # the rasterizer's exact edge and subpixel rules.
    upload_p4_test_page(0)
    upload_p4_test_page(64)
    palette_a = [0, 0x001f, 0x03e0, 0x7c00, 0x03ff] + [0] * 11
    palette_b = [0, 0x7fe0, 0x7c1f, 0x7fff, 0x4210] + [0] * 11
    upload_palette(32, 800, palette_a)
    upload_palette(48, 801, palette_b)
    clut_a = (800 << 6) | (32 >> 4)
    clut_b = (801 << 6) | (48 >> 4)
    uv_tri = ((0, 0), (15, 0), (0, 7))
    uv_quad = ((0, 0), (15, 0), (0, 7), (15, 7))
    ft3(160, 160, uv_tri, 0x0800, clut_a)
    ft3(160, 80, uv_tri, 0x0800, clut_b)
    ft4(240, 160, uv_quad, 0x0801, clut_b)
    for x, y, index in ((176, 168, 9), (208, 168, 10), (168, 208, 11),
                        (176, 88, 12), (208, 88, 13), (168, 128, 14),
                        (248, 168, 15), (296, 168, 16),
                        (248, 216, 17), (296, 216, 18)):
        c0_pixel(x, y, index)

    if texture_window:
        # E2 mask+offset moves the masked U bit to one, selecting the right
        # half of the 16x16 four-quadrant indexed texture. The lower sample
        # also checks the bottom-right quadrant. Reset E2 and repeat a fresh
        # polygon to prove subsequent packets use the unwindowed UVs.
        # Keep the baseline polygon oracle on its original 8-row texture;
        # extend the texture only after all baseline C0 reads have completed.
        upload_p4_test_page(0, 16)
        upload_p4_test_page(64, 16)
        gp0(0xe2000000 | 1 | (1 << 10))
        ft3(20, 80, ((0, 0), (15, 0), (0, 15)), 0x0800, clut_b)
        for x, y, index in ((36, 88, 19), (68, 88, 20), (28, 120, 21)):
            c0_pixel(x, y, index)
        gp0(0xe2000000)
        ft3(20, 160, ((0, 0), (15, 0), (0, 15)), 0x0800, clut_b)
        for x, y, index in ((36, 168, 22), (68, 168, 23), (28, 200, 24)):
            c0_pixel(x, y, index)

    if sprite_uv_wrap:
        # Sample one interior pixel from each broad color patch while a
        # 272x16 sprite starting at UV(252,252) crosses U and V seams, then
        # wraps U again after a complete 256-texel period.
        upload_p4_wrap_quadrants()
        emit_uv_wrap_sprite(24, 80, 0x0000, clut_a)
        for x, y, index in ((26, 82, 19), (34, 82, 20),
                            (26, 90, 21), (34, 90, 22), (284, 82, 23)):
            c0_pixel(x, y, index)

        palette_p8 = [0] * 256
        palette_p8[1:5] = palette_b[1:5]
        upload_rect(64, 800, 256, 1, palette_p8)
        upload_p8_wrap_quadrants()
        clut_p8 = (800 << 6) | (64 >> 4)
        emit_uv_wrap_sprite(24, 112, 0x0080, clut_p8)
        for x, y, index in ((26, 114, 24), (34, 114, 25),
                            (26, 122, 26), (34, 122, 27), (284, 114, 28)):
            c0_pixel(x, y, index)

        upload_p16_wrap_quadrants()
        emit_uv_wrap_sprite(24, 144, 0x0100, 0)
        for x, y, index in ((26, 146, 29), (34, 146, 30),
                            (26, 154, 31), (34, 154, 32), (284, 146, 33)):
            c0_pixel(x, y, index)

    if vram_x_wrap:
        # PSX VRAM addresses wrap at 1024 words. Page X=960 crosses that
        # seam at U=128 in P8 and U=64 in P16. P8 CLUT X=1008 plus index 20
        # advances into the next VRAM row at X=4.
        palette16_a = [0] * 16
        palette16_b = [0] * 16
        palette16_a[1] = 0x7fe0
        palette16_b[4] = 0x7c1f
        upload_rect(1008, 400, 16, 1, palette16_a)
        upload_rect(0, 401, 16, 1, palette16_b)

        upload_constant_words(1020, 32, 4, 32, 0x0101)
        upload_constant_words(0, 32, 4, 32, 0x0101)
        ft4(32, 40, ((120, 40), (136, 40), (120, 40), (136, 40)),
            0x008f, (400 << 6) | (1008 >> 4))
        for x, index in ((56, 19), (72, 20)):
            c0_pixel(x, 72, index)

        upload_constant_words(0, 32, 4, 32, 0x1414)
        emit_variable_sprite(32, 120, 120, 40, 64, 16,
                             0x008f, (400 << 6) | (1008 >> 4))
        for x, index in ((38, 21), (42, 22)):
            c0_pixel(x, 128, index)
        upload_constant_words(8, 32, 4, 32, 0x1414)
        emit_variable_sprite(32, 152, 16, 40, 16, 16,
                             0x0080, (400 << 6) | (1008 >> 4))
        c0_pixel(38, 160, 23)

        upload_constant_words(1016, 32, 8, 32, 0x001f)
        upload_constant_words(0, 32, 8, 32, 0x03e0)
        ft4(128, 40, ((56, 40), (72, 40), (56, 40), (72, 40)), 0x010f, 0)
        for x, index in ((152, 24), (168, 25)):
            c0_pixel(x, 72, index)
        emit_variable_sprite(128, 120, 56, 40, 16, 16, 0x010f, 0)
        for x, index in ((134, 26), (138, 27)):
            c0_pixel(x, 128, index)

    if coord_alias:
        # Canonical x=32/y=32 primitives establish red control pixels.
        # Reissue the same shapes with upper coordinate bits set (0x4010 /
        # 0x8010-style aliases), then with low 11-bit -1 plus E5 offset 33.
        # Every alias should land over the control and replace it with green.
        clut = (800 << 6) | (32 >> 4)

        def xy_word(x, y, x_upper=0, y_upper=0):
            return ((x | x_upper) & 0xffff) | (((y | y_upper) & 0xffff) << 16)

        def coord_set(x, y, alias):
            if alias == "negative":
                return [xy_word(0x7ff, 0x7ff), xy_word(63, 0x7ff),
                        xy_word(0x7ff, 63)]
            x_upper, y_upper = (0x4000, 0x8000) if alias == "upper" else (0, 0)
            return [xy_word(x, y, x_upper, y_upper),
                    xy_word(x + 64, y, x_upper, y_upper),
                    xy_word(x, y + 64, x_upper, y_upper)]

        def coord_set_quad(x, y, alias):
            points = coord_set(x, y, alias)
            if alias == "negative":
                return points + [xy_word(63, 63)]
            return points + [xy_word(x + 64, y + 64,
                                     0x4000 if alias == "upper" else 0,
                                     0x8000 if alias == "upper" else 0)]

        probes = (
            ("ft3", 32, 32, (48, 40), 19),
            ("ft4", 128, 32, (144, 40), 22),
            ("sprite", 32, 120, (34, 122), 25),
        )
        for kind, x, y, (sample_x, sample_y), result_index in probes:
            draw_env(0, 0, 319, 239)
            if kind == "ft3":
                draw = lambda alias, u: ft3_raw(coord_set(x, y, alias),
                    ((u, 0),) * 3, 0x0800, clut)
            elif kind == "ft4":
                draw = lambda alias, u: ft4_raw(coord_set_quad(x, y, alias),
                    ((u, 0),) * 4, 0x0800, clut)
            else:
                def draw(alias, u):
                    xy = (xy_word(x, y, 0x4000, 0x8000) if alias == "upper"
                          else xy_word(0x7ff, 0x7ff) if alias == "negative"
                          else xy_word(x, y))
                    if alias == "negative":
                        gp0(0xe5000000 | 33 | (33 << 11))
                    emit_variable_sprite_raw(xy, u, 0, 16, 16, 0x0800, clut)
                    if alias == "negative":
                        gp0(0xe5000000)
            draw("canonical", 0)
            c0_pixel(sample_x, sample_y, result_index)
            draw("upper", 8)
            c0_pixel(sample_x, sample_y, result_index + 1)
            if kind != "sprite":
                draw_env(0, 0, 319, 239, offset_x=33, offset_y=33)
                draw("negative", 8)
                draw_env(0, 0, 319, 239)
            else:
                draw("negative", 8)
            c0_pixel(sample_x, sample_y, result_index + 2)

    if coord_span_cull:
        # A 1024-pixel GP0 triangle edge exceeds the PSX rasterizer's maximum
        # X span. NEON rejects it in prepare_triangle; MAME rejects its edge
        # in CULLPOINT. The GL bridge must apply the same primitive cull before
        # rasterizing, or this oversized triangle overwrites the red witness.
        draw_env(0, 0, 319, 239)
        tile8(160, 80, 0x001f)
        c0_pixel(160, 80, 19)
        gp0(0x2000ff00)
        gp0(0x00140600)  # (-512, 20), low 11-bit X = 0x600
        gp0(0x00140200)  # ( 512, 20), low 11-bit X = 0x200
        gp0(0x00c80000)  # (   0,200); witness is well inside the triangle
        c0_pixel(160, 80, 20)

        # The adjacent 1023-span case remains a valid triangle and must draw.
        tile8(160, 80, 0x001f)
        c0_pixel(160, 80, 21)
        gp0(0x2000ff00)
        gp0(0x00140600)  # (-512, 20)
        gp0(0x001401ff)  # ( 511, 20), span is exactly 1023
        gp0(0x00c80000)
        c0_pixel(160, 80, 22)

        # For an FT4 split along (v1,v2), cull the first span-1024 triangle
        # but retain its valid second triangle. The two read points isolate
        # each half away from the diagonal and top/side edges.
        tile8(96, 56, 0x001f)
        tile8(264, 56, 0x001f)
        c0_pixel(100, 60, 23)
        c0_pixel(270, 60, 24)
        gp0(0x2800ff00)
        gp0(0x00140600)  # v0=(-512,20)
        gp0(0x00140200)  # v1=( 512,20)
        gp0(0x00640000)  # v2=(   0,100)
        gp0(0x00780014)  # v3=(  20,120)
        c0_pixel(100, 60, 25)
        c0_pixel(270, 60, 26)

        # The same split test with FT4 ensures a culled half does not lose
        # the surviving half's texture coordinates, TPAGE, or CLUT.
        p4_span_palette = [0, 0x001f, 0x03e0] + [0] * 13
        upload_constant_words(64, 512, 4, 8, 0x2222)
        upload_palette(32, 800, p4_span_palette)
        ft4_coords = (0x00140600, 0x00140200,
                      0x00640000, 0x00780014)
        ft4_uv = ((0, 0), (0, 0), (0, 0), (0, 0))
        tile8(96, 56, 0x001f)
        tile8(264, 56, 0x001f)
        c0_pixel(100, 60, 27)
        c0_pixel(270, 60, 28)
        ft4_raw(ft4_coords, ft4_uv, 0x0801, (800 << 6) | (32 >> 4))
        c0_pixel(100, 60, 29)
        c0_pixel(270, 60, 30)

        # A fully culled FT3 still updates the global TPAGE, as the NEON
        # parser does before its rasterizer rejects the triangle. Follow it
        # with a sprite without E1 so page 1's green texels distinguish that
        # state from the explicitly selected page 0 with red texels.
        upload_constant_words(0, 512, 4, 8, 0x1111)
        gp0(0xe1000800)
        ft3_raw((0x01900600, 0x01900200, 0x01f40000),
                ((0, 0), (0, 0), (0, 0)),
                0x0801, (800 << 6) | (32 >> 4))
        gp0(0x64000000 | 0x808080)
        gp0(32 | (130 << 16))
        gp0(((800 << 6) | (32 >> 4)) << 16)
        gp0(8 | (8 << 16))
        c0_pixel(34, 132, 31)

        # A Gouraud G4 with red v0 and green v1/v2/v3 has the same split.
        # The surviving triangle must start from v1's green command color.
        tile8(264, 56, 0x001f)
        gp0(0x380000ff)
        gp0(0x00140600)  # v0=(-512,20), red
        gp0(0x0000ff00)  # v1=green
        gp0(0x00140200)
        gp0(0x0000ff00)  # v2=green
        gp0(0x00640000)
        gp0(0x0000ff00)  # v3=green
        gp0(0x00780014)
        c0_pixel(270, 60, 32)

    li(9, RESULT_MAGIC)
    code.append(drc.sw(9, 0, 16))
    loop = 0xbfc01000 + len(code) * 4
    code.extend((drc.j(loop), drc.nop()))
    return code, commands


def make_regions(path, texture_window=False, sprite_uv_wrap=False, vram_x_wrap=False,
                 coord_alias=False, coord_span_cull=False):
    """Reuse the DRC's local System12 board/media regions; replace guest ROM."""
    drc = module("system12_drc_fixture", ROOT / "tests/check_system12_drc.py")
    drc.make_regions(path, 4, vita_signature=True)
    code, _ = make_guest_code(texture_window=texture_window,
                              sprite_uv_wrap=sprite_uv_wrap,
                              vram_x_wrap=vram_x_wrap,
                              coord_alias=coord_alias,
                              coord_span_cull=coord_span_cull)
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
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--texture-window", action="store_true",
                       help="add the optional E2 mask/offset and reset probe")
    modes.add_argument("--vram-x-wrap", action="store_true",
                       help="probe P8 CLUT carry and P8/P16 texture-page X seams")
    modes.add_argument("--coord-alias", action="store_true",
                       help="probe FT3, FT4, and sprite 11-bit coordinate aliases")
    modes.add_argument("--coord-span-cull", action="store_true",
                       help="probe oversized FT3/F4/FT4/G4 X spans and adjacent valid cases")
    args = parser.parse_args()
    if (args.output_dir.exists() or not args.emulator.exists() or
            not args.vpk.is_file() or not args.firmware_fs.is_dir() or
            not args.shader_compiler.is_file() or
            not 0 < args.timeout < float("inf")):
        parser.error("require an existing emulator/VPK/firmware/shader compiler, fresh output directory, and positive timeout")

    checker = module("vram_checker", Path(__file__).with_name("check_vita3k.py"))
    drc = module("vram_host_helpers", Path(__file__).with_name("check_system12_drc_vita3k.py"))
    gameplay = module("vram_pack_helpers", Path(__file__).with_name("check_system12_gameplay_vita3k.py"))
    checker.CapturedSmoke.ISSUE_MARKERS += (
        b"pcsx-vita: system12-framebuffer FAIL",
        b"pcsx-vita: system12-vram-x-wrap FAIL",
        b"pcsx-vita: system12-coord-alias FAIL",
        b"pcsx-vita: system12-coord-span-cull FAIL")
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
        make_regions(region, texture_window=args.texture_window,
                     vram_x_wrap=args.vram_x_wrap,
                     coord_alias=args.coord_alias,
                     coord_span_cull=args.coord_span_cull)
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
        verify_arg, expected, run_name, expected_colors = probe_configuration(
            texture_window=args.texture_window,
            vram_x_wrap=args.vram_x_wrap,
            coord_alias=args.coord_alias,
            coord_span_cull=args.coord_span_cull)
        passed = installed and checker.run_owned(base + ["--installed-path", TITLE_ID,
            "--app-args", f"pcsx-vita, --system12, ux0:app/{TITLE_ID}/framebuffer, "
            f"{verify_arg}, --frames, 1"], log, args.timeout,
            run_name, expected, require_ari64=True)
        evidence = dict(passed=passed, expected=expected.pattern,
            colors_rgb555=list(expected_colors),
            polygon_colors_rgb555=list(EXPECTED_POLYGON_PIXELS),
            texture_window_colors_rgb555=(list(EXPECTED_TEXTURE_WINDOW_PIXELS)
                                          if args.texture_window else None),
            main_sha256=gameplay.sha256_file(region / "main.bin"),
            host_sha256=gameplay.sha256_file(emulator),
            vpk_sha256=gameplay.sha256_file(args.vpk),
            shader_compiler_sha256=gameplay.sha256_file(args.shader_compiler),
            log=str(log.resolve()))
        (args.output_dir / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    run_label = ("System12 coordinate-span culling" if args.coord_span_cull else
                 "System12 framebuffer coherence")
    print(f"Vita3K {run_label}: {'PASS' if passed else 'FAIL'}; log={log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
