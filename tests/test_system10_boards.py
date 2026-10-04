import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import prepare_mame_psx as mame
import prepare_system10_board as system10
import system10_boards


def rom_load(name: str, data: bytes, offset: int = 0) -> str:
    crc = f"{zlib.crc32(data) & 0xffffffff:08x}"
    sha = hashlib.sha1(data).hexdigest()
    return f'ROM_LOAD("{name}", {offset:#x}, {len(data):#x}, CRC({crc}) SHA1({sha}))'


def synthetic_profile(profile, regions):
    cloned = dict(profile)
    cloned["rom_regions"] = [{"tag": tag, "size": len(data)} for tag, data in regions.items()]
    return cloned


class System10RegistryTests(unittest.TestCase):
    def test_registry_covers_all_45_pinned_mame_and_inventory_entries(self):
        commit, profiles = system10.load_profiles()
        self.assertEqual(commit, mame.PINNED_MAME_COMMIT)
        self.assertEqual(len(profiles), 45)
        self.assertEqual(system10_boards.build_registry(), json.loads(
            (ROOT / "scripts/system10_boards.json").read_text()))
        self.assertEqual({p["board_type"] for p in profiles.values()},
                         {"MEM(M)", "MEM(N)", "MEM(P3)", "MEM/IO"})
        self.assertEqual(sum(p["board_type"] == "MEM(M)" for p in profiles.values()), 3)
        self.assertEqual(sum(p["board_type"] == "MEM(N)" for p in profiles.values()), 37)
        self.assertEqual(sum(p["board_type"] == "MEM(P3)" for p in profiles.values()), 3)
        self.assertEqual(sum(p["board_type"] == "MEM/IO" for p in profiles.values()), 2)

        source = ROOT.parent / "mamedev/src/mame/namco/namcos10.cpp"
        rows = system10_boards._game_rows(source.read_text())
        self.assertEqual(set(rows), set(profiles))
        for name, profile in profiles.items():
            system10._verify_profile(name, profile, source)
        self.assertEqual(profiles["mrdrilr2j"]["crypto_profile"], "mrdrilr2")
        self.assertEqual(profiles["gamsharao"]["crypto_profile"], "gamshara")
        self.assertTrue(profiles["chocovdr"]["runtime_candidate"])
        self.assertEqual(profiles["mrdrilr2"]["rom_regions"][1]["tag"], "nand")
        self.assertEqual(profiles["mrdrilr2"]["rom_regions"][2]["tag"], "data")
        self.assertEqual(profiles["gjspace"]["rom_regions"][1]["size"], 0x1080000)
        self.assertIn("EXIO", profiles["ptblank3"]["extensions"])
        self.assertIn("MEM(P3)", profiles["g13jnr"]["extensions"][0])
        self.assertIn("CHD/disc media", profiles["taiko2"]["unsupported_requirements"])
        self.assertTrue(profiles["ippo"]["no_dump_files"])

    def test_profiles_refuse_upstream_not_working_and_board_extensions(self):
        _, profiles = system10.load_profiles()
        for set_name in ("puzzball", "taiko2", "ptblank3", "g13jnr", "ippo"):
            with self.subTest(set_name=set_name), self.assertRaises(system10.System10PreparationError):
                system10._refuse_unsupported(set_name, profiles[set_name])
        tampered = dict(profiles["ptblank3"])
        tampered["unsupported_requirements"] = []
        with self.assertRaisesRegex(system10.System10PreparationError, "differs from the complete"):
            system10._verify_profile("ptblank3", tampered,
                                     ROOT.parent / "mamedev/src/mame/namco/namcos10.cpp")


class System10MediaTests(unittest.TestCase):
    def test_raw_memn_pages_and_memm_regions_keep_exact_region_bytes(self):
        _, profiles = system10.load_profiles()
        memn_regions = {
            "maincpu:rom": b"MAIN", "nand0": bytes(range(256)) * 2 + b"OOB!123456789012",
            "nand1": b"second-chip",
        }
        memn = system10._make_payloads(synthetic_profile(profiles["chocovdr"], memn_regions), memn_regions)
        payloads, names = memn
        self.assertEqual(names, {"maincpu:rom": "main.bin", "nand0": "rawnand0.bin",
                                 "nand1": "rawnand1.bin"})
        self.assertEqual(payloads["rawnand0.bin"], bytes(range(256)) * 2 + b"OOB!123456789012")
        self.assertEqual(payloads["rawnand1.bin"], b"second-chip")

        memm_regions = {
            "maincpu:rom": b"boot", "nand": b"flash-image", "data": b"data-rom",
        }
        memm_payloads, memm_names = system10._make_payloads(
            synthetic_profile(profiles["mrdrilr2"], memm_regions), memm_regions)
        self.assertEqual(memm_names, {"maincpu:rom": "main.bin", "nand": "rawflash.bin",
                                      "data": "datarom.bin"})
        self.assertEqual(memm_payloads["rawflash.bin"], b"flash-image")
        self.assertEqual(memm_payloads["datarom.bin"], b"data-rom")

    def test_missing_or_unimplemented_regions_fail_closed(self):
        _, profiles = system10.load_profiles()
        valid = {"maincpu:rom": b"m", "nand0": b"n"}
        profile = synthetic_profile(profiles["chocovdr"], {**valid, "nand1": b"n"})
        with self.assertRaisesRegex(system10.System10PreparationError, "missing"):
            system10._make_payloads(profile, valid)
        bad = {**valid, "nand1": b"n", "decrypter:nonlinear_table": b"x"}
        with self.assertRaisesRegex(system10.System10PreparationError, "unsupported"):
            system10._make_payloads(synthetic_profile(profiles["chocovdr"], {**valid, "nand1": b"n"}), bad)

    def test_packs_synthetic_media_digests_and_preserves_other_output_files(self):
        _, profiles = system10.load_profiles()
        regions = {"maincpu:rom": b"ABCD", "nand0": bytes(range(256)) * 2 + b"OOB!123456789012"}
        profile = synthetic_profile(profiles["chocovdr"], regions)
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "board"
            output.mkdir()
            (output / "user-note.txt").write_text("keep")
            manifest = system10.package_regions("chocovdr", profile, regions, output,
                                                 {"rom_set": "chocovdr"})
            self.assertEqual((output / "main.bin").read_bytes(), b"ABCD")
            self.assertEqual((output / "rawnand0.bin").read_bytes(), regions["nand0"])
            self.assertEqual((output / "user-note.txt").read_text(), "keep")
            self.assertIn("crypto_profile=chocovdr", (output / "board.ini").read_text())
            self.assertFalse(manifest["board_execution_verified"])
            self.assertEqual(json.loads((output / "manifest.json").read_text()), manifest)
            for filename, record in (("main.bin", manifest["regions"]["maincpu:rom"]),
                                     ("rawnand0.bin", manifest["regions"]["nand0"])):
                data = (output / filename).read_bytes()
                self.assertEqual(record["size"], len(data))
                self.assertEqual(record["crc32"], f"{zlib.crc32(data) & 0xffffffff:08x}")
                self.assertEqual(record["sha1"], hashlib.sha1(data).hexdigest())

    def test_generic_assembler_loads_verified_parent_archive_without_rom_renaming(self):
        shared = b"shared raw NAND bytes"
        main = b"boot"
        source_text = f'''ROM_START(parent)
          ROM_REGION(4, "maincpu:rom", 0) {rom_load("main.bin", main)}
          ROM_REGION32_LE(0x210, "nand0", 0) {rom_load("shared.bin", shared)}
        ROM_END
        ROM_START(child)
          ROM_REGION(4, "maincpu:rom", 0) {rom_load("main.bin", main)}
          ROM_REGION32_LE(0x210, "nand0", 0) {rom_load("shared.bin", shared)}
        ROM_END
        GAME(2000, parent, 0, machine, input, state, init, ROT0, "Maker", "Parent", 0)
        GAME(2000, child, parent, machine, input, state, init, ROT0, "Maker", "Child", 0)
        '''
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            src = root / "mame"
            for driver in mame.DRIVERS:
                path = src / "src/mame" / driver
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source_text if driver == system10.DRIVER else "")
            rompath = root / "roms"
            rompath.mkdir()
            with zipfile.ZipFile(rompath / "parent.zip", "w") as archive:
                archive.writestr("shared.bin", shared)
            with zipfile.ZipFile(rompath / "child.zip", "w") as archive:
                archive.writestr("main.bin", main)
            regions, metadata = system10._load_regions("child", src, rompath, None)
            self.assertEqual(regions, {"maincpu:rom": main, "nand0": shared + bytes(0x210 - len(shared))})
            self.assertEqual(metadata["rom_files"][1]["provided_by"], str((rompath / "parent.zip").resolve()))


if __name__ == "__main__":
    unittest.main()
