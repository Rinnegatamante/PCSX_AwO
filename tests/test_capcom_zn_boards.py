from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import capcom_zn_boards as profiles
import prepare_capcom_zn_board as prepare
import prepare_mame_psx as mame


class CapcomZNRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = profiles.build_registry()
        cls.by_set = {row["set"]: row for row in cls.document["profiles"]}

    def test_registry_covers_all_capcom_family_games_and_models(self):
        self.assertEqual(self.document["mame_source_commit"], mame.PINNED_MAME_COMMIT)
        self.assertEqual(self.document["family_game_count"], 47)
        self.assertEqual(len(self.by_set), 47)
        self.assertEqual(self.document["bios_roots"], ["coh1000c", "coh3002c"])
        self.assertEqual(self.document["board_model_counts"], {
            "COH1000C": 10, "COH1002C": 8, "COH3002C": 29,
        })
        self.assertEqual(sum(row["runtime_candidate"] for row in self.by_set.values()), 43)
        self.assertEqual(sum(row["state"] == "glpracr_state" for row in self.by_set.values()), 2)

    def test_machine_geometry_and_game_parent_bios_are_source_derived(self):
        a = self.by_set["ts2"]
        self.assertEqual((a["board_model"], a["cpu_device"], a["cpu_clock_hz"],
                          a["gpu_device"], a["gpu_clock_hz"], a["main_ram_size"],
                          a["vram_size"]),
                         ("COH1000C", "CXD8530CQ", 67737600,
                          "CXD8561Q", 53693175, 0x400000, 0x100000))
        self.assertEqual(a["bios_parent"], "coh1000c")
        self.assertEqual(a["default_bios"], "j")
        self.assertEqual(a["bios_options"], {"0": "j", "1": "e"})
        b = self.by_set["sfex"]
        self.assertEqual((b["board_model"], b["vram_size"]), ("COH1002C", 0x200000))
        c = self.by_set["rvschool"]
        self.assertEqual((c["board_model"], c["cpu_device"], c["cpu_clock_hz"],
                          c["gpu_device"], c["main_ram_size"], c["vram_size"]),
                         ("COH3002C", "CXD8661R", 100000000,
                          "CXD8654Q", 0x400000, 0x200000))
        self.assertEqual(c["bios_parent"], "coh3002c")

    def test_only_exact_unused_undumped_region_is_reviewed(self):
        review = self.by_set["ts2"]["optional_undumped_media_review"]
        self.assertEqual(review["tag"], "upd78081")
        self.assertEqual(review["size"], 0x2000)
        self.assertEqual(review["filename"], "78081g503.ic655")
        self.assertEqual(review["handling"], "omitted_from_media_package_no_substitute")
        self.assertIn("ZNMCU", review["source_proof"])
        self.assertIn("undumped", review["known_limitation"])
        self.assertIsNone(self.by_set["glpracr"]["optional_undumped_media_review"])
        self.assertFalse(self.by_set["glpracr"]["runtime_candidate"])

    def test_qsound_rom_ignore_retains_full_source_file_digest_length(self):
        q = self.document["qsound_dsp"]
        self.assertEqual(q["source"], "devices/sound/qsoundhle.cpp")
        self.assertEqual((q["size"], q["file_size"], q["ignored_bytes"]),
                         (0x2000, 0x6000, 0x4000))
        self.assertEqual(q["crc32"], "d6cf5ef5")
        self.assertEqual(q["sha1"], "555f50fe5cdf127619da7d854c03f4a244a0c501")


class CapcomZNPackagingTests(unittest.TestCase):
    def test_package_emits_regions_and_preserves_unrelated_files(self):
        profile = profiles.build_registry()["profiles"]
        profile = next(row for row in profile if row["set"] == "ts2")
        regions = {tag: bytes([index]) * size for index, (tag, size) in enumerate([
            ("maincpu:rom", 0x80000), ("cat702_1", 8), ("cat702_2", 8),
            ("countryrom", 0x80000), ("bankedroms", 0x2400000),
            ("audiocpu", 0x40000), ("qsound", 0x400000),
        ])}
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / "package"
            out.mkdir()
            (out / "keep.txt").write_text("preserve")
            result = prepare.package_regions("ts2", profile, regions,
                                             b"Q" * 0x2000, out,
                                             {"selected_bios": "e", "qsound_dsp_origin": "/roms/q.zip"})
            self.assertEqual((out / "keep.txt").read_text(), "preserve")
            self.assertEqual((out / "main.bin").read_bytes(), regions["maincpu:rom"])
            self.assertEqual((out / "qsound_dsp.bin").read_bytes(), b"Q" * 0x2000)
            self.assertIn("bios=e", (out / "board.ini").read_text())
            manifest = json.loads((out / "manifest.json").read_text())
            self.assertFalse(manifest["board_execution_verified"])
            self.assertEqual(manifest["omitted_media"][0]["filename"], "78081g503.ic655")
            self.assertEqual(manifest["qsound_dsp_source"]["provided_by"], "/roms/q.zip")
            self.assertEqual(result["regions"]["cat702_key1.bin"]["size"], 8)
            for changed in ({**regions, "maincpu:rom": b"bad"},
                            {**regions, "unknown": b"bad"}):
                with self.assertRaises(prepare.CapcomPreparationError):
                    prepare.package_regions("ts2", profile, changed,
                                            b"Q" * 0x2000, out, {})
            self.assertEqual((out / "main.bin").read_bytes(), regions["maincpu:rom"])

    def test_package_rejects_unimplemented_subclass_and_missing_media(self):
        all_profiles = profiles.build_registry()["profiles"]
        glpracr = next(row for row in all_profiles if row["set"] == "glpracr")
        with self.assertRaises(prepare.CapcomPreparationError):
            prepare.package_regions("glpracr", glpracr, {}, b"Q" * 0x2000,
                                    Path("unused"), {})
        ts2 = next(row for row in all_profiles if row["set"] == "ts2")
        with self.assertRaises(prepare.CapcomPreparationError):
            prepare.package_regions("ts2", ts2, {}, b"Q" * 0x2000,
                                    Path("unused"), {})

    def test_only_reviewed_upd78081_region_is_filtered(self):
        text = (profiles.DEFAULT_SOURCE / "src/mame" / profiles.DRIVER).read_text()
        sets, _, _ = mame.parse_driver(text, profiles.DRIVER)
        original = sets["ts2"]
        ts2 = next(row for row in profiles.build_registry()["profiles"] if row["set"] == "ts2")
        filtered = prepare._filter_reviewed_unused_mcu(original, ts2)
        self.assertNotIn("upd78081", filtered.regions)
        self.assertIn("cat702_1", filtered.regions)
        self.assertIn("cat702_2", filtered.regions)
        self.assertIn("upd78081", original.regions)
        glpracr = next(row for row in profiles.build_registry()["profiles"] if row["set"] == "glpracr")
        with self.assertRaises(prepare.CapcomPreparationError):
            prepare._filter_reviewed_unused_mcu(sets["glpracr"], glpracr)

    def test_bios_selection_uses_mame_default_or_explicit_choice(self):
        text = (profiles.DEFAULT_SOURCE / "src/mame" / profiles.DRIVER).read_text()
        sets, _, _ = mame.parse_driver(text, profiles.DRIVER)
        self.assertEqual(mame._selected_bios(sets["ts2"], None), "j")
        self.assertEqual(mame._selected_bios(sets["ts2"], "e"), "e")
        with self.assertRaises(mame.PreparationError):
            mame._selected_bios(sets["ts2"], "x")
        with self.assertRaises(mame.PreparationError):
            mame._selected_bios(sets["sfex2p"], "j")


if __name__ == "__main__":
    unittest.main()
