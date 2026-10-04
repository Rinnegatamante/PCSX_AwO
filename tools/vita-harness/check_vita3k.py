#!/usr/bin/env python3
"""Run the DRC/GPU smoke guest in Vita3K and close the owned host process."""
import argparse
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--renderer", choices=("neon", "vitagl"), default="neon")
    parser.add_argument("--vpk", type=Path, help="install this VPK and launch it")
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.frames <= 216000 or args.timeout <= 0:
        parser.error("invalid frame count or timeout")
    title = "PCSXN0012" if args.renderer == "neon" else "PCSXG0012"
    command = [str(args.emulator.resolve()), "--keep-config", "--log-level", "1"]
    command += [str(args.vpk.resolve())] if args.vpk else ["--installed-path", title]
    command += ["--app-args", f"pcsx-vita, ux0:app/{title}/smoke.exe, --frames, {args.frames}, --verify-smoke"]
    expected = re.compile(r"pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b runs="
                          + str(args.frames) + r"\b")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with args.log.open("xb") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + args.timeout
        completed_at = None
        passed = False
        try:
            while time.monotonic() < deadline:
                log = args.log.read_text(errors="replace")
                if ("Unhandled EXC_" in log or "|C|" in log or
                        "pcsx-vita: smoke FAIL" in log):
                    break
                passed = bool(expected.search(log)) and "Stopping -> Idle" in log
                if passed:
                    if completed_at is None:
                        completed_at = time.monotonic()
                    # Observe shutdown before closing Vita3K, which normally
                    # keeps its host UI open after the guest application exits.
                    if time.monotonic() - completed_at >= 1:
                        break
                if process.poll() is not None:
                    passed = passed and process.returncode == 0
                    break
                time.sleep(0.05)
            else:
                passed = False
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        # Also check output flushed between the final observation and exit.
        log = args.log.read_text(errors="replace")
        passed = passed and "Unhandled EXC_" not in log and "|C|" not in log
    print(f"Vita3K {args.renderer}: {'PASS' if passed else 'FAIL'}; log={args.log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
