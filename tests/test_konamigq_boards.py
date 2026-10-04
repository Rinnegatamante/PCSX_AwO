"""GQ source and media boundaries; no authentic media or boot is fabricated."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import konamigq_boards as boards


class GQRegistryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = json.loads(boards.DEFAULT_OUTPUT.read_text())
        cls.profile = cls.registry["profiles"][0]
        cls.raw = (boards.DEFAULT_SOURCE / "src/mame" / boards.DRIVER).read_text()

    def test_complete_pinned_family(self):
        self.assertEqual(self.registry, boards.build_registry())
        self.assertEqual(1, self.registry["family_game_count"])
        self.assertEqual("cryptklr", self.profile["set"])
        self.assertFalse(self.profile["game_execution_verified"])

    def test_rom_transforms_and_writable_hdd(self):
        regions = {region["tag"]: region for region in self.profile["rom_regions"]}
        self.assertEqual({"soundcpu", "k054539", "maincpu:rom"}, set(regions))
        load = regions["soundcpu"]["loads"][0]
        self.assertEqual((2, 2, True), (load["group"], load["stride"], load["swap"]))
        self.assertEqual("420a01.2g", load["filename"])
        disk = self.profile["disk_media"][0]
        self.assertEqual("420uaa04", disk["name"])
        self.assertEqual("67cb1418fc0de2a89fc61847dc9efb9f1bebb347", disk["sha1"])
        self.assertFalse(disk["readonly"])
        self.assertEqual(0, self.profile["scsi_target_id"])

    def test_source_default_eeprom_is_not_a_rom_requirement(self):
        words = self.profile["eeprom_default_words"]
        self.assertEqual(64, len(words))
        storage = b"".join(word.to_bytes(2, "little") for word in words)
        self.assertEqual(b"\x2b\x29\x56\x52", storage[:4])
        self.assertEqual(b"\xaa" * 102, storage[26:])
        self.assertEqual(self.profile["eeprom_default_sha256"], hashlib.sha256(storage).hexdigest())
        self.assertNotIn("eeprom", {region["tag"] for region in self.profile["rom_regions"]})

    def test_changed_source_geometry_and_eeprom_are_rejected(self):
        with self.assertRaises(boards.ProfileError):
            boards.source_facts(self.raw.replace('set_default_size("4M")', 'set_default_size("2M")'))
        with self.assertRaises(boards.ProfileError):
            boards.source_facts(self.raw.replace("0x292b,", ""))

    def test_wrong_inventory_family_and_commit_are_rejected(self):
        original = json.loads(boards.INVENTORY.read_text())
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "inventory.json"
            changed = json.loads(json.dumps(original))
            changed["source"]["mame_commit"] = "invalid"
            path.write_text(json.dumps(changed))
            with self.assertRaises(boards.ProfileError):
                boards.build_registry(inventory_path=path)
            changed = json.loads(json.dumps(original))
            next(f for f in changed["systems"] if f["driver"] == boards.DRIVER)["sets"] = []
            path.write_text(json.dumps(changed))
            with self.assertRaises(boards.ProfileError):
                boards.build_registry(inventory_path=path)


if __name__ == "__main__":
    unittest.main()
