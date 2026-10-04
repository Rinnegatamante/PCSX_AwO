import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

MODULE_PATH = (Path(__file__).resolve().parents[1] /
               "tools/vita-harness/check_system12_gameplay_vita3k.py")
SPEC = importlib.util.spec_from_file_location("system12_vita_gameplay", MODULE_PATH)
GAMEPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GAMEPLAY)


class System12VitaGameplayEvidenceTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix", "process-group watchdog uses POSIX sessions")
    def test_watchdog_kills_owned_group_even_when_caller_is_stalled(self):
        with tempfile.TemporaryDirectory() as directory:
            child_pid_file = Path(directory) / "child.pid"
            child_signal_file = Path(directory) / "child.sigterm"
            child_code = (
                "import signal,time,pathlib; "
                f"signal.signal(signal.SIGTERM, lambda *_: pathlib.Path({str(child_signal_file)!r}).write_text('term')); "
                "time.sleep(30)")
            parent_code = (
                "import pathlib,subprocess,sys,time; "
                f"child=subprocess.Popen([sys.executable,'-c',{child_code!r}]); "
                f"pathlib.Path({str(child_pid_file)!r}).write_text(str(child.pid)); "
                "time.sleep(30)")
            process = subprocess.Popen([sys.executable, "-c", parent_code],
                                       start_new_session=True)
            watchdog = GAMEPLAY.ProcessGroupWatchdog(process, 0.15)
            try:
                deadline = time.monotonic() + 3
                while not child_pid_file.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(child_pid_file.exists())
                time.sleep(2.0)  # Simulate a stalled caller through TERM and forced KILL.
            finally:
                watchdog.close()
            self.assertTrue(watchdog.timed_out)
            self.assertIsNotNone(process.poll())
            self.assertTrue(child_signal_file.exists())

    def test_png_screenshot_config_is_explicit_and_unique(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yml"
            path.write_text("pref-path: /tmp/fs\nscreenshot-format: 1\n")
            GAMEPLAY.set_png_screenshot_format(path)
            self.assertIn("screenshot-format: 2\n", path.read_text())
            path.write_text("screenshot-format: 0\nscreenshot-format: 1\n")
            with self.assertRaisesRegex(RuntimeError, "duplicate screenshot-format"):
                GAMEPLAY.set_png_screenshot_format(path)

    def test_native_screenshot_copy_requires_real_png_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "portable" / "screenshots"
            source = root / "title" / "frame.png"
            source.parent.mkdir(parents=True)
            valid_header = (b"\x89PNG\r\n\x1a\n" + (13).to_bytes(4, "big") + b"IHDR" +
                            (1).to_bytes(4, "big") * 2 + bytes((8, 6, 0, 0, 0)))
            source.write_bytes(valid_header + b"fixture")
            output = Path(directory) / "out"
            output.mkdir()
            copied = GAMEPLAY.copy_new_screenshots(
                root, set(), output, 400)
            self.assertEqual(len(copied), 1)
            self.assertEqual(Path(copied[0]["path"]).read_bytes(), source.read_bytes())
            source.write_bytes(b"not a png")
            bad_output = Path(directory) / "bad"
            bad_output.mkdir()
            with self.assertRaisesRegex(RuntimeError, "signature/IHDR"):
                GAMEPLAY.copy_new_screenshots(root, set(), bad_output, 800)

    def test_short_guest_markers_are_visible_and_late_fatal_invalidates(self):
        fragments = ("pcsx-vita: system12-report PASS",)
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vita.log"
            code = ("import time; print('pcsx-vita: system12-report PASS',flush=True); "
                    "print('Stopping -> Idle',flush=True); time.sleep(10)")
            process = subprocess.Popen([sys.executable, "-u", "-c", code],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, log, "unit pass")
            try:
                self.assertTrue(captured.wait_for_messages(fragments, 2, stable=0.15))
            finally:
                process.terminate()
                process.wait(timeout=5)
                captured.close()

            code = ("import time; print('pcsx-vita: system12-report PASS',flush=True); "
                    "print('Stopping -> Idle',flush=True); time.sleep(.15); "
                    "print('[InterpreterFallback] Unimplemented instruction at 0x1',flush=True); "
                    "time.sleep(10)")
            process = subprocess.Popen([sys.executable, "-u", "-c", code],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, log, "unit late fatal")
            try:
                self.assertFalse(captured.wait_for_messages(fragments, 2, stable=0.5))
            finally:
                process.terminate()
                process.wait(timeout=5)
                captured.close()
            self.assertEqual(captured.issue_counts[
                "[InterpreterFallback] Unimplemented instruction"], 1)

    def test_guest_log_filter_bounds_repeated_arm_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vita.log"
            code = (
                "import sys; "
                "[print('[InterpreterFallback] Unimplemented instruction at 0x81155B94') "
                "for _ in range(30000)]; "
                "print('pcsx-vita: system12-report PASS path=report.txt'); "
                "print('Stopping -> Idle')")
            process = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, log, "unit flood")
            process.wait(timeout=10)
            captured.close()
            self.assertEqual(captured.issue_counts[
                "[InterpreterFallback] Unimplemented instruction"], 30000)
            self.assertIn("Stopping -> Idle", captured.markers)
            self.assertLess(log.stat().st_size, 2 * 1024 * 1024)

    def test_ppm_dimensions_and_payload_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.ppm"
            path.write_bytes(b"P6\n2 1\n255\n\xff\x00\x00\x00\xff\x00")
            self.assertEqual(GAMEPLAY.check_capture(path)["width"], 2)
            self.assertEqual(GAMEPLAY.check_capture(path)["sampled_unique_rgb"], 2)
            self.assertFalse(GAMEPLAY.check_capture(path)["looks_black"])
            path.write_bytes(b"P6\n2 1\n255\n\x00")
            with self.assertRaisesRegex(RuntimeError, "dimensions or payload"):
                GAMEPLAY.check_capture(path)
            path.write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")
            self.assertTrue(GAMEPLAY.check_capture(path)["looks_black"])

    def test_report_requires_live_board_and_h8_progress(self):
        report = (
            "System12: profile=soulclbrac machine=coh700 state=namcos12_state RAM=4194304 "
            "main=4194304 banked=33554432 sub=524288 wave=16777216 bank=0 alt=0\n"
            "System12: H8 pc=000100 er7=00000000 ccr=00 cycles=100 instructions=20 "
            "halted=0 illegal=000000:0000 unsupported=0 shared r=1 w=2\n"
            "System12: H8 SCI0=00/00/00 SCI1=00/00/00 IRQ=00/00/00 portB=ff\n"
            "System12: C352 rate=88200 frames=300 peak=100 WAV_bytes=0\n")
        report += (
            "System12: unmapped reads=0 writes=0 last=00000000 pc=00000000\n"
            "System12: CPU pc=80010000 opcode=00000000 ra=00000000; clock=50000000 "
            "cycle_multiplier=200 timing=source-experiment\n")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "board.txt"
            path.write_text(report)
            parsed = GAMEPLAY.read_report(path)
            self.assertEqual(parsed["records"]["H8"]["cycles"], "100")
            self.assertEqual(parsed["records"]["H8"]["instructions"], "20")
            path.write_text(report.replace("unsupported=0", "unsupported=1"))
            with self.assertRaisesRegex(RuntimeError, "H8 did not make"):
                GAMEPLAY.read_report(path)
            path.write_text(report.replace("machine=coh700", "machine=wrong"))
            with self.assertRaisesRegex(RuntimeError, "wrong live board profile"):
                GAMEPLAY.read_report(path)


if __name__ == "__main__":
    unittest.main()
