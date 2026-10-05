#!/usr/bin/env python3
"""Host-side checks for the generated System12 framebuffer guest fixture."""
import importlib.util
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
        self.assertEqual(commands.count(0xc0000000), 8)
        self.assertIn(0xa0000000, commands)
        self.assertIn(0x80000000, commands)
        self.assertEqual(FIXTURE.EXPECTED_PIXELS,
                         (0x001f, 0x03e0, 0x001f, 0x7c00,
                          0x7fff, 0x03e0, 0x7c1f, 0x7c1f))

    def test_regions_reuse_board_media_and_replace_only_main_guest(self):
        with tempfile.TemporaryDirectory(prefix="system12-framebuffer-test-") as temp:
            region = Path(temp)
            FIXTURE.make_regions(region)
            self.assertEqual((region / "main.bin").stat().st_size, 0x400000)
            self.assertTrue((region / "banked.bin").is_file())
            self.assertTrue((region / "sub.bin").is_file())
            self.assertTrue((region / "wave.bin").is_file())
            board = (region / "board.cfg").read_text(encoding="ascii")
            self.assertIn("ram_size=4194304", board)
            self.assertIn("machine=coh700", board)


if __name__ == "__main__":
    unittest.main()
