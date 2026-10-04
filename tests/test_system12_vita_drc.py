#!/usr/bin/env python3
"""Unit checks for the Vita System12 DRC guest signatures and log gate."""
import importlib.util
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fixture = load_module("system12_drc_fixture", ROOT / "tests/check_system12_drc.py")
runner = load_module("system12_vita_drc_runner", ROOT / "tools/vita-harness/check_system12_drc_vita3k.py")


class System12VitaDrcTests(unittest.TestCase):
    def test_guest_contains_signature_stores_for_both_ram_profiles(self):
        for ram_mb in (4, 16):
            with self.subTest(ram_mb=ram_mb):
                main, _ = fixture.make_guest(ram_mb, vita_signature=True)
                words = struct.unpack_from("<%dI" % (0x1000 // 4), main, 0x1000)
                required = (
                    fixture.lui(9, 0x5331),
                    fixture.ori(9, 9, 0x3244),
                    fixture.sw(16, 8, 8), fixture.sw(17, 12, 8),
                    fixture.sw(18, 16, 8), fixture.sw(19, 20, 8),
                    fixture.sw(20, 24, 8), fixture.sw(21, 28, 8),
                    fixture.sw(22, 32, 8), fixture.sw(23, 36, 8),
                    fixture.sw(24, 40, 8), fixture.sw(25, 44, 8),
                    fixture.sw(24, 40, 8), fixture.sw(25, 44, 8),
                )
                self.assertTrue(all(word in words for word in required))
                if ram_mb == 4:
                    self.assertIn(fixture.addu(8, 12, 0), words)
                    self.assertNotIn(fixture.addu(8, 16, 0), words)

    def test_vita_probe_output_requires_cpu_ram_and_all_guest_results(self):
        line4 = ("pcsx-vita: system12-drc PASS cpu=ari64 ram=4M magic=53313244 "
                 "bank0=00001111 bank1=00002222 upper=51a7cafe alias=00001357 "
                 "smc=00001234/00005678 dma5=00003333 mmio=76543210 "
                 "half=00001357 byte=00000089 runs=1")
        line16 = line4.replace("ram=4M", "ram=16M").replace("alias=00001357", "alias=00002468")
        self.assertTrue(runner.system12_result_matches(line4, 4))
        self.assertTrue(runner.system12_result_matches(line16, 16))
        self.assertFalse(runner.system12_result_matches(line4.replace("ari64", "interpreter"), 4))
        self.assertFalse(runner.system12_result_matches(line4.replace("00005678", "00000000"), 4))
        self.assertFalse(runner.system12_result_matches(line4, 16))

    def test_guest_success_requires_idle_after_signature(self):
        line = ("pcsx-vita: system12-drc PASS cpu=ari64 ram=4M magic=53313244 "
                "bank0=00001111 bank1=00002222 upper=51a7cafe alias=00001357 "
                "smc=00001234/00005678 dma5=00003333 mmio=76543210 "
                "half=00001357 byte=00000089 runs=1")
        self.assertFalse(runner.completed_guest_matches(line, 4))
        self.assertTrue(runner.completed_guest_matches(line + "\nStopping -> Idle", 4))

    def test_runner_rejects_crash_after_signature_and_closes_owned_process(self):
        line = ("pcsx-vita: system12-drc PASS cpu=ari64 ram=4M magic=53313244 "
                "bank0=00001111 bank1=00002222 upper=51a7cafe alias=00001357 "
                "smc=00001234/00005678 dma5=00003333 mmio=76543210 "
                "half=00001357 byte=00000089 runs=1\n"
                "Stopping -> Idle\n")
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "vita3k.log"
            with log_path.open("w") as output:
                process = subprocess.Popen(
                    [sys.executable, "-c",
                     "import sys,time; print(sys.argv[1], end='', flush=True); "
                     "time.sleep(.25); print('Unhandled EXC_BAD_ACCESS', flush=True); "
                     "time.sleep(10)", line], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            passed = runner.run_until(process, log_path,
                                      lambda text: runner.completed_guest_matches(text, 4),
                                      3, "test crash")
            self.assertFalse(passed)
            self.assertIsNotNone(process.poll())
            self.assertIn("Unhandled EXC_BAD_ACCESS", log_path.read_text())

    def test_runner_closes_owned_process_on_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "vita3k.log"
            with log_path.open("w") as output:
                process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            started = time.monotonic()
            passed = runner.run_until(process, log_path, lambda text: False,
                                      0.15, "test timeout")
            self.assertFalse(passed)
            self.assertLess(time.monotonic() - started, 3)
            self.assertIsNotNone(process.poll())

    def test_runner_does_not_accept_a_previous_guest_signature(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "vita3k.log"
            log_path.write_text("old guest success\nStopping -> Idle\n")
            process = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(10)"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            self.assertFalse(runner.run_until(
                process, log_path, lambda text: "old guest success" in text,
                0.15, "previous guest"))
            self.assertIsNotNone(process.poll())

    def test_runner_uses_unique_temporary_portable_app(self):
        self.assertNotEqual(runner.TITLE_ID, "PCSX00001")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = root / "Vita3K.app"
            binary = bundle / "Contents/MacOS/Vita3K"
            binary.parent.mkdir(parents=True)
            binary.touch()
            found_bundle, relative = runner.locate_app_bundle(binary)
            self.assertEqual(found_bundle, bundle.absolute())
            self.assertEqual(relative.as_posix(), "Contents/MacOS/Vita3K")
            outside = root / "external-vita3k"
            outside.touch()
            binary.unlink()
            binary.symlink_to(outside)
            with self.assertRaisesRegex(RuntimeError, "outside its app bundle"):
                runner.locate_app_bundle(binary)


if __name__ == "__main__":
    unittest.main()
