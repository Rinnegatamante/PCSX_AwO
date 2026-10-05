#!/usr/bin/env python3
"""Host-side checks for the generated System12 framebuffer guest fixture."""
import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PATH = ROOT / "tools/vita-harness/check_system12_framebuffer_vita3k.py"
SPEC = importlib.util.spec_from_file_location("system12_framebuffer_fixture", FIXTURE_PATH)
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


class System12FramebufferFixtureTests(unittest.TestCase):
    def test_guest_covers_onscreen_pages_clip_transfer_and_move(self):
        code, commands = FIXTURE.make_guest_code()
        registers = {0: 0}
        memory_bases_used = set()
        first_mmio_store = first_result_store = None
        for pc, word in enumerate(code):
            opcode = word >> 26
            rs, rt, immediate = (word >> 21) & 31, (word >> 16) & 31, word & 0xffff
            if opcode == 0x0f:  # LUI
                registers[rt] = immediate << 16
            elif opcode == 0x0d:  # ORI
                registers[rt] = registers[rs] | immediate if rs in registers else None
            elif opcode in (0x23, 0x2b):  # LW/SW
                memory_bases_used.add(rs)
                self.assertIn(rs, (8, 16))
                self.assertIn(rs, registers)
                self.assertEqual(registers[rs], 0xbf801810 if rs == 8 else 0x8023f100)
                if opcode == 0x2b and rs == 8 and first_mmio_store is None:
                    first_mmio_store = pc
                if opcode == 0x2b and rs == 16 and first_result_store is None:
                    first_result_store = pc
                if opcode == 0x23:
                    registers.pop(rt, None)
            elif opcode == 0x0c:  # ANDI writes the read-result temporary.
                registers.pop(rt, None)
        self.assertEqual(memory_bases_used, {8, 16})
        self.assertIsNotNone(first_mmio_store)
        self.assertIsNotNone(first_result_store)
        self.assertLess(first_mmio_store, first_result_store)
        # The actual MIPS setup words must compose the fixed hardware/RAM
        # addresses before their first memory use, including guest result RAM.
        self.assertEqual((registers.get(8), registers.get(16)),
                         (0xbf801810, 0x8023f100))

        clips = []
        pending_start = None
        for word in commands:
            opcode = word >> 24
            if opcode == 0xe3:
                pending_start = (word & 0x3ff, (word >> 10) & 0x1ff)
            elif opcode == 0xe4 and pending_start is not None:
                x2, y2 = word & 0x3ff, (word >> 10) & 0x1ff
                clips.append((x2 - pending_start[0] + 1, y2 - pending_start[1] + 1))
                pending_start = None
        self.assertEqual(clips, [(320, 240), (320, 240), (320, 220), (320, 240)])
        self.assertTrue(all(w * 4 >= FIXTURE.DISPLAY_W * 3 and
                            h * 4 >= FIXTURE.DISPLAY_H * 3 for w, h in clips))
        self.assertIn(0xe5000000 | (240 << 11), commands)
        self.assertEqual(commands.count(0xc0000000), 18)
        self.assertNotIn(0xe2000401, commands)
        self.assertNotIn(0xe2000000, commands)
        gp0_sha256 = hashlib.sha256(b"".join(word.to_bytes(4, "little")
                                               for word in commands)).hexdigest()
        self.assertEqual(gp0_sha256, FIXTURE.DEFAULT_GP0_SHA256)
        self.assertIn(0xa0000000, commands)
        self.assertIn(0x80000000, commands)
        polygons = []
        c0_points = []
        cursor = 0
        current_clip = None
        draw_area_start = None
        while cursor < len(commands):
            word = commands[cursor]
            opcode = word >> 24
            if opcode == 0xe3:
                draw_area_start = (word & 0x3ff, (word >> 10) & 0x1ff)
                cursor += 1
            elif opcode == 0xe4 and draw_area_start is not None:
                x2, y2 = word & 0x3ff, (word >> 10) & 0x1ff
                current_clip = (draw_area_start[0], draw_area_start[1],
                                x2 - draw_area_start[0] + 1,
                                y2 - draw_area_start[1] + 1)
                draw_area_start = None
                cursor += 1
            elif opcode == 0xa0:
                pixel_count = ((commands[cursor + 2] & 0xffff) *
                               (commands[cursor + 2] >> 16))
                cursor += 3 + (pixel_count + 1) // 2
            elif opcode in (0x24, 0x2c):
                length = 7 if opcode == 0x24 else 9
                clut = commands[cursor + 2] >> 16
                tpage = commands[cursor + 4] >> 16
                uv_offsets = (2, 4, 6) if opcode == 0x24 else (2, 4, 6, 8)
                uvs = tuple(commands[cursor + offset] & 0xffff for offset in uv_offsets)
                self.assertEqual(len(set(uvs)), len(uvs))
                polygons.append((opcode, tpage, clut, current_clip, uvs))
                cursor += length
            elif opcode == 0x80:
                cursor += 4
            elif opcode == 0xc0:
                xy = commands[cursor + 1]
                c0_points.append((xy & 0xffff, xy >> 16))
                cursor += 3
            elif opcode == 0x70:
                cursor += 2
            else:
                cursor += 1
        expected_uv_tri = (0, 15, 7 << 8)
        expected_uv_quad = (0, 15, 7 << 8, 15 | (7 << 8))
        self.assertEqual(polygons, [
            (0x24, 0x0800, (800 << 6) | 2, (0, 0, 320, 240), expected_uv_tri),
            (0x24, 0x0800, (801 << 6) | 3, (0, 0, 320, 240), expected_uv_tri),
            (0x2c, 0x0801, (801 << 6) | 3, (0, 0, 320, 240), expected_uv_quad),
        ])
        self.assertEqual(c0_points[-10:],
                         [(176, 168), (208, 168), (168, 208),
                          (176, 88), (208, 88), (168, 128),
                          (248, 168), (296, 168), (248, 216), (296, 216)])
        self.assertEqual(FIXTURE.EXPECTED_POLYGON_PIXELS,
                         (0x001f, 0x03e0, 0x7c00, 0x7fe0, 0x7c1f,
                          0x7fff, 0x7fe0, 0x7c1f, 0x7fff, 0x4210))
        self.assertEqual(FIXTURE.EXPECTED_PIXELS,
                         (0x001f, 0x03e0, 0x001f, 0x7c00,
                          0x7fff, 0x03e0, 0x7c1f, 0x7c1f))

    def test_regions_reuse_board_media_and_replace_only_main_guest(self):
        with tempfile.TemporaryDirectory(prefix="system12-framebuffer-test-") as temp:
            region = Path(temp)
            FIXTURE.make_regions(region)
            self.assertEqual((region / "main.bin").stat().st_size, 0x400000)
            self.assertEqual(hashlib.sha256((region / "main.bin").read_bytes()).hexdigest(),
                             FIXTURE.DEFAULT_MAIN_SHA256)
            self.assertTrue((region / "banked.bin").is_file())
            self.assertTrue((region / "sub.bin").is_file())
            self.assertTrue((region / "wave.bin").is_file())
            board = (region / "board.cfg").read_text(encoding="ascii")
            self.assertIn("ram_size=4194304", board)
            self.assertIn("machine=coh700", board)

            optional_region = region / "texture-window"
            optional_region.mkdir()
            FIXTURE.make_regions(optional_region, texture_window=True)
            self.assertEqual(hashlib.sha256(
                (optional_region / "main.bin").read_bytes()).hexdigest(),
                FIXTURE.TEXTURE_WINDOW_MAIN_SHA256)

            wrap_region = region / "sprite-uv-wrap"
            wrap_region.mkdir()
            FIXTURE.make_regions(wrap_region, sprite_uv_wrap=True)
            self.assertEqual(hashlib.sha256(
                (wrap_region / "main.bin").read_bytes()).hexdigest(),
                FIXTURE.SPRITE_UV_WRAP_MAIN_SHA256)

            vram_edge_region = region / "vram-x-wrap"
            vram_edge_region.mkdir()
            FIXTURE.make_regions(vram_edge_region, vram_x_wrap=True)
            self.assertEqual(hashlib.sha256(
                (vram_edge_region / "main.bin").read_bytes()).hexdigest(),
                FIXTURE.VRAM_X_WRAP_MAIN_SHA256)

            coord_alias_region = region / "coord-alias"
            coord_alias_region.mkdir()
            FIXTURE.make_regions(coord_alias_region, coord_alias=True)
            self.assertEqual(hashlib.sha256(
                (coord_alias_region / "main.bin").read_bytes()).hexdigest(),
                FIXTURE.COORD_ALIAS_MAIN_SHA256)

    def test_optional_texture_window_probe_sets_and_resets_e2(self):
        _code, commands = FIXTURE.make_guest_code(texture_window=True)
        self.assertEqual(commands.count(0xc0000000), 24)
        self.assertEqual([word for word in commands if word >> 24 == 0xe2],
                         [0xe2000401, 0xe2000000])

        polygons = []
        read_points = []
        cursor = 0
        while cursor < len(commands):
            opcode = commands[cursor] >> 24
            if opcode == 0xa0:
                pixel_count = ((commands[cursor + 2] & 0xffff) *
                               (commands[cursor + 2] >> 16))
                cursor += 3 + (pixel_count + 1) // 2
            elif opcode == 0x24:
                polygons.append((cursor, tuple(commands[cursor + i] & 0xffff
                                               for i in (2, 4, 6))))
                cursor += 7
            elif opcode == 0x2c:
                cursor += 9
            elif opcode == 0xc0:
                xy = commands[cursor + 1]
                read_points.append((xy & 0xffff, xy >> 16))
                cursor += 3
            elif opcode == 0x80:
                cursor += 4
            elif opcode == 0x70:
                cursor += 2
            else:
                cursor += 1

        self.assertEqual([uv for _, uv in polygons[-2:]],
                         [(0, 15, 15 << 8), (0, 15, 15 << 8)])
        first_window_polygon = next(pos for pos, _ in polygons if pos >
                                    commands.index(0xe2000401))
        reset_position = commands.index(0xe2000000)
        second_window_polygon = next(pos for pos, _ in polygons if pos > reset_position)
        self.assertLess(commands.index(0xe2000401), first_window_polygon)
        self.assertLess(reset_position, second_window_polygon)
        self.assertEqual(read_points[-6:], [(36, 88), (68, 88), (28, 120),
                                           (36, 168), (68, 168), (28, 200)])
        self.assertEqual(FIXTURE.EXPECTED_TEXTURE_WINDOW_PIXELS,
                         (0x7c1f, 0x7c1f, 0x4210, 0x7fe0, 0x7c1f, 0x7fff))

    def test_optional_sprite_uv_wrap_covers_p4_p8_and_p16_seams(self):
        _code, commands = FIXTURE.make_guest_code(sprite_uv_wrap=True)
        self.assertEqual(commands.count(0xc0000000), 33)
        self.assertEqual([word for word in commands if word >> 24 == 0xe1],
                         [0xe1000000, 0xe1000080, 0xe1000100])
        gp0_sha256 = hashlib.sha256(b"".join(word.to_bytes(4, "little")
                                               for word in commands)).hexdigest()
        self.assertEqual(gp0_sha256, FIXTURE.SPRITE_UV_WRAP_GP0_SHA256)

        sprites = []
        reads = []
        tpage = None
        cursor = 0
        while cursor < len(commands):
            word = commands[cursor]
            opcode = word >> 24
            if opcode == 0xa0:
                pixel_count = ((commands[cursor + 2] & 0xffff) *
                               (commands[cursor + 2] >> 16))
                cursor += 3 + (pixel_count + 1) // 2
            elif opcode == 0xe1:
                tpage = word & 0x1ff
                cursor += 1
            elif opcode == 0x64:
                xy = commands[cursor + 1]
                sprites.append((tpage, xy & 0xffff, xy >> 16,
                                commands[cursor + 2] & 0xffff,
                                commands[cursor + 3]))
                cursor += 4
            elif opcode == 0xc0:
                xy = commands[cursor + 1]
                reads.append((xy & 0xffff, xy >> 16))
                cursor += 3
            elif opcode in (0x24, 0x2c):
                cursor += 7 if opcode == 0x24 else 9
            elif opcode == 0x80:
                cursor += 4
            elif opcode == 0x70:
                cursor += 2
            else:
                cursor += 1

        self.assertEqual(sprites, [
            (0x0000, 24, 80, 0xfcfc, 0x00100110),
            (0x0080, 24, 112, 0xfcfc, 0x00100110),
            (0x0100, 24, 144, 0xfcfc, 0x00100110),
        ])
        self.assertEqual(reads[-15:], [(26, 82), (34, 82), (26, 90), (34, 90), (284, 82),
                                       (26, 114), (34, 114), (26, 122), (34, 122), (284, 114),
                                       (26, 146), (34, 146), (26, 154), (34, 154), (284, 146)])
        self.assertEqual(FIXTURE.EXPECTED_SPRITE_UV_WRAP_PIXELS,
                         (0x03ff, 0x7c00, 0x03e0, 0x001f, 0x7c00,
                          0x4210, 0x7fff, 0x7c1f, 0x7fe0, 0x7fff,
                          0x03ff, 0x7c00, 0x03e0, 0x001f, 0x7c00))
        with self.assertRaises(ValueError):
            FIXTURE.make_guest_code(texture_window=True, sprite_uv_wrap=True)

    def test_optional_vram_x_wrap_crosses_texture_and_clut_edges(self):
        _code, commands = FIXTURE.make_guest_code(vram_x_wrap=True)
        self.assertEqual(commands.count(0xc0000000), 27)
        self.assertEqual([word for word in commands if word >> 24 == 0xe1],
                         [0xe100008f, 0xe1000080, 0xe100010f])
        gp0_sha256 = hashlib.sha256(b"".join(word.to_bytes(4, "little")
                                               for word in commands)).hexdigest()
        self.assertEqual(gp0_sha256, FIXTURE.VRAM_X_WRAP_GP0_SHA256)

        uploads = []
        sprites = []
        polygon_tpages = []
        reads = []
        tpage = None
        cursor = 0
        while cursor < len(commands):
            word = commands[cursor]
            opcode = word >> 24
            if opcode == 0xa0:
                xy, size = commands[cursor + 1], commands[cursor + 2]
                width, height = size & 0xffff, size >> 16
                uploads.append((xy & 0xffff, xy >> 16, width, height))
                cursor += 3 + (width * height + 1) // 2
            elif opcode == 0xe1:
                tpage = word & 0x1ff
                cursor += 1
            elif opcode == 0x64:
                xy = commands[cursor + 1]
                sprites.append((tpage, xy & 0xffff, xy >> 16,
                                commands[cursor + 2] & 0xffff,
                                commands[cursor + 3]))
                cursor += 4
            elif opcode == 0xc0:
                xy = commands[cursor + 1]
                reads.append((xy & 0xffff, xy >> 16))
                cursor += 3
            elif opcode == 0x2c:
                polygon_tpages.append(commands[cursor + 4] >> 16)
                cursor += 9
            elif opcode == 0x80:
                cursor += 4
            elif opcode == 0x70:
                cursor += 2
            else:
                cursor += 1

        self.assertEqual(sprites, [
            (0x008f, 32, 120, 120 | (40 << 8), 64 | (16 << 16)),
            (0x0080, 32, 152, 16 | (40 << 8), 16 | (16 << 16)),
            (0x010f, 128, 120, 56 | (40 << 8), 16 | (16 << 16)),
        ])
        self.assertEqual(polygon_tpages[-2:], [0x008f, 0x010f])
        self.assertIn((1008, 400, 16, 1), uploads)
        self.assertIn((0, 401, 16, 1), uploads)
        self.assertIn((1020, 32, 4, 32), uploads)
        self.assertIn((0, 32, 4, 32), uploads)
        self.assertIn((8, 32, 4, 32), uploads)
        self.assertIn((1016, 32, 8, 32), uploads)
        self.assertIn((0, 32, 8, 32), uploads)
        self.assertEqual(reads[-9:], [(56, 72), (72, 72), (38, 128), (42, 128),
                                     (38, 160), (152, 72), (168, 72),
                                     (134, 128), (138, 128)])
        self.assertEqual(FIXTURE.EXPECTED_VRAM_X_WRAP_PIXELS,
                         (0x7fe0, 0x7fe0, 0x7fe0, 0x7c1f, 0x7c1f,
                          0x001f, 0x03e0, 0x001f, 0x03e0))
        with self.assertRaises(ValueError):
            FIXTURE.make_guest_code(sprite_uv_wrap=True, vram_x_wrap=True)

    def test_optional_coordinate_aliases_cover_ft3_ft4_and_sprite(self):
        _code, commands = FIXTURE.make_guest_code(coord_alias=True)
        self.assertEqual(commands.count(0xc0000000), 27)
        gp0_sha256 = hashlib.sha256(b"".join(word.to_bytes(4, "little")
                                               for word in commands)).hexdigest()
        self.assertEqual(gp0_sha256, FIXTURE.COORD_ALIAS_GP0_SHA256)

        ft3_packets = []
        ft4_packets = []
        sprites = []
        reads = []
        cursor = 0
        while cursor < len(commands):
            word = commands[cursor]
            opcode = word >> 24
            if opcode == 0xa0:
                pixels = ((commands[cursor + 2] & 0xffff) *
                          (commands[cursor + 2] >> 16))
                cursor += 3 + (pixels + 1) // 2
            elif opcode == 0x24:
                ft3_packets.append(tuple(commands[cursor + offset]
                                         for offset in (1, 3, 5)))
                cursor += 7
            elif opcode == 0x2c:
                ft4_packets.append(tuple(commands[cursor + offset]
                                         for offset in (1, 3, 5, 7)))
                cursor += 9
            elif opcode == 0x64:
                sprites.append(commands[cursor + 1])
                cursor += 4
            elif opcode == 0xc0:
                xy = commands[cursor + 1]
                reads.append((xy & 0xffff, xy >> 16))
                cursor += 3
            elif opcode == 0x80:
                cursor += 4
            elif opcode == 0x70:
                cursor += 2
            else:
                cursor += 1

        self.assertEqual(ft3_packets[-3:], [
            (0x00200020, 0x00200060, 0x00600020),
            (0x80204020, 0x80204060, 0x80604020),
            (0x07ff07ff, 0x07ff003f, 0x003f07ff),
        ])
        self.assertEqual(ft4_packets[-3:], [
            (0x00200080, 0x002000c0, 0x00600080, 0x006000c0),
            (0x80204080, 0x802040c0, 0x80604080, 0x806040c0),
            (0x07ff07ff, 0x07ff003f, 0x003f07ff, 0x003f003f),
        ])
        self.assertEqual(sprites, [0x00780020, 0x80784020, 0x07ff07ff])
        self.assertEqual(reads[-9:], [(48, 40)] * 3 + [(144, 40)] * 3 +
                         [(34, 122)] * 3)
        self.assertEqual(FIXTURE.EXPECTED_COORD_ALIAS_PIXELS,
                         (0x001f, 0x03e0, 0x03e0,
                          0x001f, 0x03e0, 0x03e0,
                          0x001f, 0x03e0, 0x03e0))
        with self.assertRaises(ValueError):
            FIXTURE.make_guest_code(vram_x_wrap=True, coord_alias=True)


if __name__ == "__main__":
    unittest.main()
