import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_system11_inputs as inputs


class System11InputSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = json.loads((ROOT / "scripts/system11_boards.json").read_text())
        cls.source_path = ROOT.parent / "mamedev/src/mame/namco/namcos11.cpp"
        if cls.source_path.is_file():
            cls.source = cls.source_path.read_text()
            cls.blocks = inputs.input_blocks(cls.source)

    def test_all_registry_input_profiles_generate_deterministic_include(self):
        if not self.source_path.is_file():
            self.skipTest("local pinned MAME source is unavailable")
        rendered = inputs.generate(self.source, self.profiles)
        self.assertEqual(rendered, (ROOT / "libpcsxcore/arcade/system11_inputs.inc").read_text())
        self.assertIn('static const struct System11InputBinding input_bindings[]', rendered)
        self.assertIn('static const struct System11InputDefault input_defaults[]', rendered)
        names = {profile["input"] for profile in self.profiles["profiles"]}
        self.assertEqual(names, {"namcos11", "tekken", "souledge", "myangel3", "ptblank2ua", "pocketrc"})

    def test_tekken_and_souledge_extra_button_wiring_and_active_low_defaults(self):
        if not self.source_path.is_file():
            self.skipTest("local pinned MAME source is unavailable")
        tekken = inputs._expand("tekken", self.blocks, {})
        souledge = inputs._expand("souledge", self.blocks, {})
        self.assertIn(inputs.Control("p1button4", 0xff, None, True), tekken["ADC1"].controls)
        self.assertIn(inputs.Control("p1button3", 0xff, None, True), tekken["ADC2"].controls)
        self.assertIn(inputs.Control("p2button3", 0x10, 2, False), tekken["PLAYER4"].controls)
        self.assertIn(inputs.Control("p2button4", 0x20, 2, False), tekken["PLAYER4"].controls)
        self.assertIn(inputs.Control("p1button4", 0xff, None, True), souledge["ADC2"].controls)
        self.assertEqual(tekken["SWITCH"].value, 0xff)
        self.assertEqual(tekken["PLAYER1"].value, 0xff)

    def test_all_four_players_coins_dips_unused_and_special_profiles(self):
        if not self.source_path.is_file():
            self.skipTest("local pinned MAME source is unavailable")
        base = inputs._expand("namcos11", self.blocks, {})
        self.assertEqual([c.control for c in base["SWITCH"].controls],
                         ["service", "test", "coin1", "coin2", "coin3", "coin4", "dip1_test", "dip2_freeze"])
        self.assertEqual(base["SWITCH"].value, 0xff)
        for player, port in ((1, "PLAYER1"), (2, "PLAYER2"), (4, "PLAYER4")):
            self.assertIn(f"p{player}button3", {c.control for c in base[port].controls})
            self.assertIn(f"p{player}start", {c.control for c in base[port].controls})
        self.assertIn("p3button1", {c.control for c in base["ADC2"].controls})
        myangel = inputs._expand("myangel3", self.blocks, {})
        self.assertEqual({c.control for c in myangel["PLAYER1"].controls if "button" in c.control},
                         {"p1button1", "p1button2", "p1button3", "p1button4"})
        self.assertFalse(myangel["PLAYER4"].controls)
        pocket = inputs._expand("pocketrc", self.blocks, {})
        self.assertEqual(pocket["ADC0"].value, 0x80)
        self.assertEqual(pocket["ADC1"].value, 0)
        self.assertIn("p4button2", {c.control for c in pocket["PLAYER4"].controls})

    def test_gun_ports_keep_exact_sixteen_bit_defaults(self):
        if not self.source_path.is_file():
            self.skipTest("local pinned MAME source is unavailable")
        ports = inputs._expand("ptblank2ua", self.blocks, {})
        self.assertEqual([ports[name].value for name in ("GUN1X", "GUN1Y", "GUN2X", "GUN2Y")],
                         [0x022f, 0x00a8, 0x022f, 0x00a8])

    def test_synthetic_include_modify_splits_overridden_bits(self):
        source = '''
static INPUT_PORTS_START(base)
 PORT_START("SWITCH")
 PORT_BIT(0x03, IP_ACTIVE_LOW, IPT_COIN1)
 PORT_BIT(0xfc, IP_ACTIVE_LOW, IPT_UNUSED)
INPUT_PORTS_END
static INPUT_PORTS_START(child)
 PORT_INCLUDE(base)
 PORT_MODIFY("SWITCH")
 PORT_BIT(0x01, IP_ACTIVE_LOW, IPT_UNUSED)
 PORT_BIT(0x02, IP_ACTIVE_LOW, IPT_COIN2)
INPUT_PORTS_END
'''
        ports = inputs._expand("child", inputs.input_blocks(source), {})
        self.assertEqual(ports["SWITCH"].value, 0xff)
        self.assertEqual(ports["SWITCH"].controls, [inputs.Control("coin2", 0x02)])

    def test_unknown_or_malformed_input_macros_fail_closed(self):
        profiles = {"mame_source_commit": inputs.PINNED_COMMIT,
                    "profiles": [{"input": "test"}]}
        source = '''
static INPUT_PORTS_START(test)
 PORT_START("SWITCH")
 PORT_FUTURE_MACRO(0x01)
INPUT_PORTS_END
'''
        with self.assertRaisesRegex(inputs.InputSourceError, "unsupported macro"):
            inputs.generate(source, profiles)
        with self.assertRaisesRegex(inputs.InputSourceError, "does not match"):
            inputs.generate(source, {"mame_source_commit": "wrong", "profiles": []})


if __name__ == "__main__":
    unittest.main()
