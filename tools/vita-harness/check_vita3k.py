#!/usr/bin/env python3
"""Run the DRC/GPU smoke guest with an independently bounded Vita3K host."""
import argparse
import importlib.util
from pathlib import Path
import re
import signal
import subprocess
import time

_HELPER_PATH = Path(__file__).with_name("check_system12_gameplay_vita3k.py")
_HELPER_SPEC = importlib.util.spec_from_file_location("vita3k_process_capture", _HELPER_PATH)
_HELPER = importlib.util.module_from_spec(_HELPER_SPEC)
_HELPER_SPEC.loader.exec_module(_HELPER)
CapturedGuest = _HELPER.CapturedGuest
ProcessGroupWatchdog = _HELPER.ProcessGroupWatchdog


class CapturedSmoke(CapturedGuest):
    ISSUE_MARKERS = CapturedGuest.ISSUE_MARKERS + (b"pcsx-vita: smoke FAIL",)
    # Retain the process ownership record used by the fake-host regression test.
    KEY_WORDS = CapturedGuest.KEY_WORDS + (b"owned host pid=",)


def fatal(log):
    """Compatibility for the synthetic DRC checker importing this module."""
    return any(marker.decode("ascii") in log for marker in CapturedSmoke.ISSUE_MARKERS)


def smoke_complete(captured, expected):
    return (any(expected.search(message) for message in captured.messages) and
            "Stopping -> Idle" in captured.markers)


def wait_for_completion(captured, expected, timeout):
    deadline = time.monotonic() + timeout
    completed_at = None
    while time.monotonic() < deadline:
        if any(captured.issue_counts.values()):
            return False
        complete = smoke_complete(captured, expected)
        returncode = captured.process.poll()
        if returncode is not None:
            # Drain pending output before evaluating a host that exits normally.
            captured.thread.join(timeout=1)
            return returncode == 0 and smoke_complete(captured, expected)
        if complete:
            if completed_at is None:
                completed_at = time.monotonic()
            if time.monotonic() - completed_at >= 1:
                return True
        else:
            completed_at = None
        time.sleep(0.05)
    return False


def run_owned(command, log, timeout, label, expected, installation=False):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=True)
    watchdog = ProcessGroupWatchdog(process, timeout)
    captured = CapturedSmoke(process, log, label)
    natural_exit = False
    try:
        if installation:
            passed = captured.wait_for_messages((expected,), timeout,
                                               stable=0.25, require_idle=False)
        else:
            passed = wait_for_completion(captured, expected, timeout)
    finally:
        natural_exit = process.poll() is not None
        watchdog.close()
        captured.close()
    if installation and natural_exit and process.returncode == 0:
        passed = any(expected in message for message in captured.messages)
    accepted = (0, -signal.SIGTERM, -signal.SIGKILL)
    return (passed and not watchdog.timed_out and not any(captured.issue_counts.values()) and
            process.returncode in accepted and (not natural_exit or process.returncode == 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--renderer", choices=("neon", "vitagl"), default="neon")
    parser.add_argument("--vpk", type=Path, help="install this VPK and launch it")
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.frames <= 216000 or not 0 < args.timeout < float("inf"):
        parser.error("invalid frame count or timeout")
    title = "PCSXN0012" if args.renderer == "neon" else "PCSXG0012"
    base_command = [str(args.emulator.resolve()), "--keep-config", "--log-level", "1"]
    command = base_command + ["--installed-path", title, "--app-args",
        f"pcsx-vita, ux0:app/{title}/smoke.exe, --frames, {args.frames}, --verify-smoke"]
    expected = re.compile(r"pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 sentinel=534d4f4b runs="
                          + str(args.frames) + r"\b")
    args.log.parent.mkdir(parents=True, exist_ok=True)
    # Refuse to mix evidence with an older run; each host has a fresh capture.
    args.log.open("xb").close()
    if args.vpk:
        # Vita3K recognizes Windows-style /options on macOS as well.
        installed = run_owned(base_command + ["--", str(args.vpk.resolve())], args.log,
                              args.timeout, f"install {title}",
                              f"[{title}] installed successfully!", installation=True)
        if not installed:
            print(f"Vita3K {args.renderer}: FAIL during installation; log={args.log}")
            return 1
    passed = run_owned(command, args.log, args.timeout, f"{args.renderer} smoke", expected)
    print(f"Vita3K {args.renderer}: {'PASS' if passed else 'FAIL'}; log={args.log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
