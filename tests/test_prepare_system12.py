import hashlib
import importlib.util
import tempfile
import sys
import unittest
import zlib
import zipfile
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "prepare_system12.py"
SPEC = importlib.util.spec_from_file_location("prepare_system12", SCRIPT)
prepare_system12 = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = prepare_system12
SPEC.loader.exec_module(prepare_system12)


def declaration(filename, data, offset=0, macro="ROM_LOAD"):
    crc = f"{zlib.crc32(data) & 0xffffffff:08x}"
    sha1 = hashlib.sha1(data).hexdigest()
    return f'{macro}( "{filename}", {offset:#x}, {len(data):#x}, CRC({crc}) SHA1({sha1}) )'


class PrepareSystem12Tests(unittest.TestCase):
    def test_bundled_manifest_covers_all_eight_revisions_without_mame_checkout(self):
        romsets, commit = prepare_system12.read_static_manifest()
        self.assertEqual(len(romsets), 8)
        self.assertEqual(commit, "deb2f54fe7300e7a192690318beb860159bfd52a")
        self.assertEqual({romset.name for romset in romsets}, {
            "soulclbr", "soulclbrab", "soulclbrac", "soulclbrja2",
            "soulclbrjb", "soulclbrjc", "soulclbrub", "soulclbruc",
        })

    def test_parses_soul_calibur_mame_region_loads(self):
        source = '''
        ROM_START(soulclbrac)
          ROM_REGION32_LE(0x400000, "maincpu:rom", 0)
          ROM_LOAD16_BYTE( "lo.bin", 0, 2, CRC(00000000) SHA1(0000000000000000000000000000000000000000) )
          ROM_REGION32_LE(0x2000000, "bankedroms", 0)
          ROM_LOAD( "bank.bin", 0, 4, CRC(00000000) SHA1(0000000000000000000000000000000000000000) )
          ROM_REGION(0x80000, "sub", 0)
          ROM_LOAD16_WORD_SWAP( "sub.bin", 0, 4, CRC(00000000) SHA1(0000000000000000000000000000000000000000) )
          ROM_REGION(0x1000000, "c352", 0)
          ROM_LOAD( "wave.bin", 0, 2, CRC(00000000) SHA1(0000000000000000000000000000000000000000) )
        ROM_END
        '''
        (romset,) = prepare_system12.parse_rom_sets(source)
        self.assertEqual(romset.name, "soulclbrac")
        self.assertEqual(romset.regions["maincpu:rom"], 0x400000)
        self.assertEqual(romset.loads["sub"][0].load_type, "16_WORD_SWAP")
        self.assertEqual(romset.loads["bankedroms"][0].length, 4)

    def test_assembles_byte_lanes_and_word_swap(self):
        lo = b"ac"
        hi = b"bd"
        main = prepare_system12._assemble_region(
            8,
            (prepare_system12.RomLoad("lo", 0, 2, "16_BYTE", "", ""),
             prepare_system12.RomLoad("hi", 1, 2, "16_BYTE", "", "")),
            {"lo": lo, "hi": hi}, "maincpu:rom",
        )
        self.assertEqual(main, b"abcd\0\0\0\0")
        sub = prepare_system12._assemble_region(
            4, (prepare_system12.RomLoad("sub", 0, 4, "16_WORD_SWAP", "", ""),),
            {"sub": b"1234"}, "sub",
        )
        self.assertEqual(sub, b"2143")

    def test_prepare_verifies_hashes_and_writes_zero_filled_regions(self):
        roms = {
            "lo": b"ab", "hi": b"cd", "bank": b"xy",
            "sub": b"1234", "wave": b"uv",
        }
        source = f'''ROM_START(soulclbrac)
ROM_REGION32_LE(0x400000, "maincpu:rom", 0)
{declaration("lo", roms["lo"], 0, "ROM_LOAD16_BYTE")}
{declaration("hi", roms["hi"], 1, "ROM_LOAD16_BYTE")}
ROM_REGION32_LE(0x2000000, "bankedroms", 0)
{declaration("bank", roms["bank"])}
ROM_REGION(0x80000, "sub", 0)
{declaration("sub", roms["sub"], macro="ROM_LOAD16_WORD_SWAP")}
ROM_REGION(0x1000000, "c352", 0)
{declaration("wave", roms["wave"])}
ROM_END
'''
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            mame = root / "mamedev"
            driver = mame / "src/mame/namco/namcos12.cpp"
            driver.parent.mkdir(parents=True)
            driver.write_text(source)
            archive_path = root / "soulclbr.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                for filename, data in roms.items():
                    archive.writestr(filename, data)
            output = root / "out"
            manifest = prepare_system12.prepare(archive_path, output, mame)

            self.assertEqual(manifest["mame_set"], "soulclbrac")
            self.assertEqual((output / "main.bin").read_bytes()[:4], b"acbd")
            self.assertEqual((output / "sub.bin").read_bytes()[:4], b"2143")
            wave = (output / "wave.bin").read_bytes()
            self.assertEqual(wave[:2], b"uv")
            self.assertEqual(wave[-1], 0)
            self.assertEqual(len(wave), 0x1000000)
            self.assertTrue((output / "manifest.json").is_file())

            with zipfile.ZipFile(archive_path, "w") as archive:
                for filename, data in roms.items():
                    archive.writestr(filename, b"bad" if filename == "wave" else data)
            with self.assertRaises(prepare_system12.PreparationError):
                prepare_system12.prepare(archive_path, output, mame)


if __name__ == "__main__":
    unittest.main()
