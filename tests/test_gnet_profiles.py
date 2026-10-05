#!/usr/bin/env python3
"""Source-pinned Taito G-Net profile generator checks."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "gnet_profiles_generate", ROOT / "scripts/gnet_profiles_generate.py")
GEN = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(GEN)


class GNetProfilesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "../mamedev/src/mame/sony/taitogn.cpp").read_text()
        cls.inventory = json.loads((ROOT / "scripts/mame_psx_systems.json").read_text())
        cls.data = GEN.registry(cls.source, cls.inventory)
        cls.by_name = {item["set"]: item for item in cls.data["profiles"]}

    def test_registry_covers_exact_inventory_and_source_sets(self):
        inventory = next(s for s in self.inventory["systems"] if s["driver"] == GEN.DRIVER)
        self.assertEqual(41, inventory["set_count"])
        self.assertEqual(41, len(self.data["profiles"]))
        self.assertEqual(41, len(self.by_name))
        self.assertEqual(2, sum(bool(p["flags"] == "MACHINE_IS_BIOS_ROOT") for p in self.data["profiles"]))
        self.assertEqual(39, sum(p["chd"] is not None for p in self.data["profiles"]))
        self.assertEqual(set(s["set"] for s in inventory["sets"]), set(self.by_name))

    def test_card_bios_chd_and_metadata_variants_are_source_exact(self):
        games = [p for p in self.data["profiles"] if p["chd"]]
        self.assertEqual(23, sum(p["card_kind"] == "TYPE1" for p in games))
        self.assertEqual(5, sum(p["card_kind"] == "TYPE2" for p in games))
        self.assertEqual(3, sum(p["card_kind"] == "CF" for p in games))
        self.assertEqual(8, sum(p["card_kind"] == "ATAFLASH" for p in games))
        self.assertEqual("aerofsp", self.by_name["aerofgtsg"]["chd"])
        self.assertEqual("mb2011", self.by_name["raystormg"]["bios_default"])
        self.assertEqual("v2", self.by_name["otenamhf"]["bios_default"])
        self.assertEqual("fixed", self.by_name["mawasunda"]["bios_default"])
        self.assertEqual(8, sum(p["bios_default"] == "mb2011" for p in games))
        self.assertEqual(2, sum(p["bios_default"] == "v2" for p in games))
        self.assertEqual(28, sum(p["bios_default"] == "v1" for p in games))
        self.assertTrue(all(p["flags"] == "MACHINE_SUPPORTS_SAVE" for p in games))
        self.assertEqual("TYPE2", self.by_name["usagi"]["card_kind"])
        self.assertEqual("taitogn_jp1", self.by_name["kollonc"]["input"])
        self.assertEqual(4, len(self.data["bios"]["coh3002t"]["options"]))
        options = {row["name"] for row in self.data["bios"]["coh3002t"]["options"]}
        self.assertEqual({"v1", "v2", "mb2009", "mb2011"}, options)
        generated = GEN.emit(self.data)
        self.assertIn('GNET_METADATA_TAGS("CIS ", "KEY ", "IDNT")', generated)
        self.assertIn('"CIS_DEFAULT_IF_MISSING;KEY_OPTIONAL_EXACT5;IDNT_OPTIONAL_RAW_512"', generated)
        self.assertIn('"CIS_DEFAULT_IF_MISSING;KEY_UNUSED;IDNT_OPTIONAL_RAW_512"', generated)

    def test_generation_is_bounded_to_the_pinned_source_and_reproducible(self):
        with self.assertRaisesRegex(GEN.ProfileError, "SHA256 mismatch"):
            GEN.registry(self.source + "\n", self.inventory)
        subprocess.run(["python3", str(ROOT / "scripts/gnet_profiles_generate.py"), "--check"],
                       cwd=ROOT, check=True)
        self.assertIn(self.data["source_sha256"], GEN.emit(self.data))
        self.assertIn("No `board.ini`/native loader profile", GEN.emit_docs(self.data))


if __name__ == "__main__":
    unittest.main()
