import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import prepare_mame_psx as mame
import prepare_system11_board as system11


class System11ProfilesTest(unittest.TestCase):
    def test_all_36_game_entries_have_exact_profile_metadata(self):
        commit, profiles = system11.load_profiles()
        self.assertEqual(commit, mame.PINNED_MAME_COMMIT)
        self.assertEqual(len(profiles), 36)
        source = ROOT.parent / "mamedev" / "src/mame/namco/namcos11.cpp"
        if source.is_file():
            rows = system11._game_rows(source.read_text(encoding="utf-8"))
            self.assertEqual(set(rows), set(profiles))
            for name, profile in profiles.items():
                system11._verify_profile_against_source(name, profile, source)
        self.assertEqual(profiles["tekken"]["model"], "COH100")
        self.assertEqual(profiles["tekken2"]["model"], "COH110")
        self.assertEqual(profiles["tekken2b"]["model"], "COH100")
        self.assertEqual(profiles["myangel3"]["banking"], "ROM8_64")
        self.assertEqual(profiles["ptblank2ua"]["keycus"], "C443")
        self.assertTrue(profiles["ptblank2ua"]["lightgun"])
        self.assertFalse(profiles["starswep"]["lightgun"])
        self.assertEqual(profiles["fambowl"]["flags"], "MACHINE_NOT_WORKING")

    def test_profiles_reject_unknown_set_and_wrong_profile_enum(self):
        _, profiles = system11.load_profiles()
        self.assertNotIn("does-not-exist", profiles)
        bad = dict(profiles["tekken"])
        bad["banking"] = "ROM64_GUESS"
        with self.assertRaisesRegex(system11.System11Error, "invalid banking"):
            # Exercise the package boundary too; it must fail before output.
            system11.package_regions("tekken", bad, {}, b"x" * 0x4000, ROOT / "test-out")


class System11AssemblerTest(unittest.TestCase):
    def _synthetic_regions(self):
        regions = {
            "maincpu:rom": mame.Region("maincpu:rom", 4, 32, "little", "", 0xff),
            "bankedroms": mame.Region("bankedroms", 4, 32, "little", "", 0xff),
            "c76": mame.Region("c76", 4, 16, "little", "", 0xff),
            "c352": mame.Region("c352", 4, 8, "native", "", 0xff),
        }
        loads = [
            mame.LoadOp(0, 0, 0, 2, 1, 2, False, "ROM_LOAD16_BYTE"),
            mame.LoadOp(1, 0, 1, 2, 1, 2, False, "ROM_LOAD16_BYTE"),
            mame.LoadOp(2, 0, 0, 4, 1, 1, False, "ROM_LOAD"),
            mame.LoadOp(3, 0, 0, 4, 1, 1, False, "ROM_LOAD"),
            mame.LoadOp(4, 0, 0, 4, 1, 1, False, "ROM_LOAD"),
        ]
        regions["maincpu:rom"].operations.extend(loads[:2])
        regions["bankedroms"].operations.append(loads[2])
        regions["c76"].operations.append(loads[3])
        regions["c352"].operations.append(loads[4])
        files = [mame.FileSpec(i, f"file{i}", 4, None, None, "good", None)
                 for i in range(5)]
        romset = mame.RomSet("synthetic", regions, files, loads, [], {}, None, "namco/namcos11.cpp")
        return mame.assemble_regions(romset, {
            0: b"AC", 1: b"BD", 2: b"bank", 3: b"prog", 4: b"wave",
        })

    def test_interleaved_regions_are_packaged_with_expected_names_and_digests(self):
        regions = self._synthetic_regions()
        _, profiles = system11.load_profiles()
        profile = dict(profiles["tekken"])
        profile["flags"] = "0"
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "tekken"
            output.mkdir()
            (output / "keep-me.txt").write_text("user file")
            manifest = system11.package_regions(
                "tekken", profile, regions, b"B" * 0x4000, output,
                {"rom_set": "tekken"}, "local/c76.zip")
            self.assertEqual({path.name for path in output.iterdir()}, {
                "main.bin", "banked.bin", "c76.bin", "c76bios.bin", "wave.bin",
                "board.ini", "manifest.json",
                "keep-me.txt",
            })
            self.assertEqual((output / "keep-me.txt").read_text(), "user file")
            self.assertEqual((output / "main.bin").read_bytes(), b"ABCD")
            self.assertEqual((output / "banked.bin").read_bytes(), b"bank")
            self.assertEqual((output / "c76.bin").read_bytes(), b"prog")
            self.assertEqual((output / "c76bios.bin").read_bytes(), b"B" * 0x4000)
            self.assertEqual((output / "wave.bin").read_bytes(), b"wave")
            self.assertEqual(manifest["c76_bios"]["sha1"], system11.EXPECTED_C76["sha1"])
            self.assertFalse(manifest["board_execution_verified"])
            self.assertIn("banking=ROM8", (output / "board.ini").read_text())
            self.assertEqual(json.loads((output / "manifest.json").read_text()), manifest)

    def test_missing_regions_and_upstream_not_working_set_fail_closed(self):
        _, profiles = system11.load_profiles()
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "out"
            with self.assertRaisesRegex(system11.System11Error, "required assembled region is missing"):
                system11.package_regions("tekken", profiles["tekken"], {}, b"B" * 0x4000, output)
            self.assertFalse(output.exists())
            with self.assertRaisesRegex(system11.System11Error, "MACHINE_NOT_WORKING"):
                system11.package_regions("fambowl", profiles["fambowl"], {}, b"B" * 0x4000, output)
            self.assertFalse(output.exists())

    def test_rom8_without_banked_payload_is_rejected(self):
        _, profiles = system11.load_profiles()
        with tempfile.TemporaryDirectory() as temp:
            regions = {"maincpu:rom": b"main", "c76": b"prog", "c352": b"wave"}
            with self.assertRaisesRegex(system11.System11Error, "has no bankedroms"):
                system11.package_regions("tekken", profiles["tekken"], regions,
                                         b"B" * 0x4000, Path(temp) / "out")

    def test_c76_internal_bios_is_mandatory(self):
        source = ROOT.parent / "mamedev"
        if not (source / "src/mame/namco/namcomcu.cpp").is_file():
            self.skipTest("local MAME source checkout is unavailable")
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(system11.System11Error, "C76 internal BIOS required"):
                system11._load_c76_bios(source, Path(temp))


if __name__ == "__main__":
    unittest.main()
