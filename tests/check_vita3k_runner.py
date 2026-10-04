#!/usr/bin/env python3
"""Check harness process ownership/completion, without emulating a Vita."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKE_HOST = r'''#!/usr/bin/env python3
from pathlib import Path
import os
import sys
import time
mode = Path(sys.argv[0]).name
print(f"owned host pid={os.getpid()}", flush=True)
if "--" in sys.argv:
    print("[install_archive_content]: test [PCSXN0012] installed successfully!", flush=True)
    # Deliberately include an old PASS: it must not validate the next launch.
    print("pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b runs=3", flush=True)
    print("Stopping -> Idle", flush=True)
else:
    if mode != "no-guest":
        print("pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b runs=3", flush=True)
    if mode != "no-idle":
        print("Stopping -> Idle", flush=True)
    if mode == "crash":
        sys.exit(17)
    if mode == "fatal":
        print("Unhandled EXC_BAD_ACCESS", flush=True)
time.sleep(30)  # A completed guest leaves its host UI open.
'''


def main():
    with tempfile.TemporaryDirectory(prefix="pcsx-vita3k-runner-") as tmp:
        root = Path(tmp)
        for mode, install, expected in (
                ("idle-host", False, 0),
                ("crash", False, 1),
                ("fatal", False, 1),
                ("no-idle", False, 1),
                ("idle-host", True, 0),
                ("no-guest", True, 1)):
            emulator = root / mode
            emulator.write_text(FAKE_HOST)
            emulator.chmod(0o700)
            log = root / f"{mode}-install{install}.log"
            command = [sys.executable, str(ROOT / "tools/vita-harness/check_vita3k.py"),
                       "--emulator", str(emulator), "--log", str(log), "--timeout", "2"]
            if install:
                vpk = root / "package.vpk"
                vpk.write_bytes(b"test package argument")
                command += ["--vpk", str(vpk)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            assert result.returncode == expected, (mode, install, result.stdout, result.stderr)
            pids = [int(line.split("=")[1]) for line in log.read_text().splitlines()
                    if line.startswith("owned host pid=")]
            assert len(pids) == (2 if install else 1)
            for pid in pids:
                # The runner must reap only the host processes it launched.
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    continue
                raise AssertionError(f"owned test host still running: {pid}")
    print("Vita3K runner completion, crash, timeout and installation isolation PASS")


if __name__ == "__main__":
    main()
