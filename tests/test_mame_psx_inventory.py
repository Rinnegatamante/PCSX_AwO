import importlib.util
import json
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "mame_psx_inventory.py"
SPEC = importlib.util.spec_from_file_location("mame_psx_inventory", SCRIPT)
mame_psx_inventory = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = mame_psx_inventory
SPEC.loader.exec_module(mame_psx_inventory)


class MamePsxInventoryTests(unittest.TestCase):
    def test_includes_playstation_soc_families_but_excludes_other_mips_and_ps2(self):
        xml = """<mame build="0.287">
          <machine name="zn_game" sourcefile="sony/zn.cpp">
            <description>ZN game</description><chip type="cpu" tag="maincpu" name="Sony CXD8530CQ" clock="67737600"/>
            <device_ref tag=":gpu" name="cxd8561q"/>
            <driver status="imperfect" emulation="good" savestate="supported"/>
            <feature type="sound" status="imperfect"/>
          </machine>
          <machine name="unrelated_r3000" sourcefile="sgi/iris.cpp">
            <description>SGI</description><chip type="cpu" tag="maincpu" name="MIPS R3000" clock="40000000"/>
          </machine>
          <machine name="ps2" sourcefile="konami/kpython.cpp">
            <description>PS2</description><chip type="cpu" tag="maincpu" name="Emotion Engine Core (big)" clock="294912000"/>
          </machine>
        </mame>"""
        result = mame_psx_inventory.inventory_from_xml(xml)
        self.assertEqual(result["completeness"]["machine_count"], 1)
        self.assertEqual(result["systems"][0]["driver"], "sony/zn.cpp")
        self.assertEqual(result["systems"][0]["sets"][0]["devices"][0]["name"], "cxd8561q")
        self.assertEqual(result["systems"][0]["sets"][0]["mame_driver"]["status"], "imperfect")
        self.assertEqual(result["systems"][0]["sets"][0]["mame_features"], [{"type": "sound", "status": "imperfect"}])
        first = json.dumps(result, ensure_ascii=False)
        second = json.dumps(mame_psx_inventory.inventory_from_xml(xml), ensure_ascii=False)
        self.assertEqual(first, second)

    def test_groups_every_known_driver_and_preserves_set_metadata(self):
        xml = """<mame build="test">
          <machine name="set_b" sourcefile="namco/namcos12.cpp" cloneof="set_a" runnable="no">
            <description>Clone set</description><chip type="cpu" tag="maincpu" name="Sony CXD8661R" clock="100000000"/>
            <device_ref tag=":gpu" name="cxd8654q"/><slot name="jvs"/>
          </machine>
          <machine name="set_a" sourcefile="namco/namcos12.cpp" isbios="yes">
            <description>BIOS root</description><chip type="cpu" tag="maincpu" name="Sony CXD8661R" clock="100000000"/>
          </machine>
          <machine name="set_c" sourcefile="konami/konamigv.cpp">
            <description>GV</description><chip type="cpu" tag="maincpu" name="Sony CXD8530BQ" clock="67737600"/>
          </machine>
        </mame>"""
        result = mame_psx_inventory.inventory_from_xml(xml)
        self.assertEqual(result["completeness"]["machine_count"], 3)
        self.assertEqual(result["completeness"]["driver_count"], 2)
        namco = next(x for x in result["systems"] if x["driver"] == "namco/namcos12.cpp")
        self.assertEqual(namco["set_count"], 2)
        clone = next(x for x in namco["sets"] if x["set"] == "set_b")
        self.assertEqual(clone["cloneof"], "set_a")
        self.assertFalse(clone["runnable"])
        self.assertEqual(clone["slots"], [{"name": "jvs"}])
        self.assertIn("config registration is not evidence", namco["support"])

    def test_real_inventory_matches_the_pinned_source_scope_and_is_sorted(self):
        path = Path(__file__).resolve().parents[1] / "scripts" / "mame_psx_systems.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(data["source"]["mame_commit"], "deb2f54fe7300e7a192690318beb860159bfd52a")
        self.assertEqual(data["completeness"]["machine_count"], 491)
        drivers = {system["driver"] for system in data["systems"]}
        self.assertEqual(drivers, {
            "sony/psx.cpp", "sony/zn.cpp", "sony/taitogn.cpp",
            "konami/konamigv.cpp", "konami/konamigq.cpp", "konami/ksys573.cpp",
            "konami/twinkle.cpp", "namco/namcos10.cpp", "namco/namcos11.cpp",
            "namco/namcos12.cpp",
        })
        for system in data["systems"]:
            names = [machine["set"] for machine in system["sets"]]
            self.assertEqual(names, sorted(names))
        sets = [machine["set"] for system in data["systems"] for machine in system["sets"]]
        self.assertEqual(len(sets), len(set(sets)))


if __name__ == "__main__":
    unittest.main()
