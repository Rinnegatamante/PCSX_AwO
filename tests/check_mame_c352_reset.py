#!/usr/bin/env python3
"""Check MAME's C352 stream/sample phase remains continuous across soft reset."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "tests/mame_c352_reset_oracle.lua"
H8_HZ = 16_934_400
SAMPLE_HZ = 88_200
ATTOSECONDS = 10**18
KEYON_CYCLE = 190
KEYON_BASE_CYCLES = 78
ATTOS_PER_H8 = ATTOSECONDS // H8_HZ


def time_as(sec: int, attos: int) -> int:
    return sec * ATTOSECONDS + attos


def run_mame(mame: str, rompath: Path, lua: Path, work: Path) -> str:
    command = [mame, "soulclbrac", "-rompath", str(rompath), "-skip_gameinfo",
               "-video", "none", "-sound", "none", "-seconds_to_run", "4",
               "-nonvram_save", "-nvram_directory", str(work / "nvram"),
               "-cfg_directory", str(work / "cfg"), "-inipath", str(work),
               "-autoboot_script", str(lua)]
    proc = subprocess.Popen(command, cwd=work, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        output, _ = proc.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        proc.send_signal(signal.SIGTERM)
        try:
            output, _ = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, _ = proc.communicate()
        raise RuntimeError(f"MAME reset oracle exceeded 45 seconds and was killed:\n{output[-8000:]}")
    if proc.returncode != 0:
        raise RuntimeError(f"MAME reset oracle exited {proc.returncode}:\n{output[-8000:]}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mame", default=os.environ.get("MAME", shutil.which("mame") or "mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    if not (args.rompath / "soulclbr.zip").exists():
        parser.error(f"requires local parent ROM archive soulclbr.zip in {args.rompath}")

    work = Path(tempfile.mkdtemp(prefix="c352-mame-reset-"))
    try:
        version = subprocess.run([args.mame, "-version"], text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=10, check=False)
        print(f"MAME {version.stdout.strip()}")
        delay = (KEYON_CYCLE - KEYON_BASE_CYCLES) // 2
        lua = work / "c352-reset.lua"
        lua.write_text(TEMPLATE.read_text().replace("__KEYON_DELAY_NOPS__", str(delay)))
        output = run_mame(args.mame, args.rompath.resolve(), lua, work)
        lines = output.splitlines()
        event_lines = [line for line in lines if "C352_RESET_EVENT" in line]
        request_match = re.search(r"C352_RESET_REQUEST sec=(\d+) attos=(\d+) flags=([0-9a-fA-F]+)", output)
        pcm_match = re.search(r"C352_RESET_PCM captured=(\d+) reset_events=(\d+) request=(\d+):(\d+) callback=(\d+):(\d+) flags=([0-9a-fA-F]+) bursts=(.*)", output)
        if not request_match or not pcm_match or len(event_lines) < 2:
            raise RuntimeError("MAME did not report the request, reset event, and two PCM bursts:\n" + "\n".join(lines[-80:]))
        sec, attos = map(int, request_match.group(1, 2))
        req_flags = int(request_match.group(3), 16)
        captured, reset_count, psec, pattos, csec, cattos = map(int, pcm_match.group(1, 2, 3, 4, 5, 6))
        final_flags = int(pcm_match.group(7), 16)
        burst_text = pcm_match.group(8)
        burst_matches = re.findall(r"(\d+)\[([^\]]*)\]", burst_text)
        bursts = [(int(index), [float(x) for x in values.split(":") if x]) for index, values in burst_matches]
        actual_reset = next((
            (int(m.group(1)), int(m.group(2)))
            for line in event_lines
            if (m := re.search(r"C352_RESET_EVENT n=2 sec=(\d+) attos=(\d+)", line))
        ), None)
        if actual_reset is None:
            raise RuntimeError("second machine reset notification was absent:\n" + "\n".join(event_lines))
        reset_attos = time_as(*actual_reset)
        predicted = (reset_attos + KEYON_CYCLE * ATTOS_PER_H8) * SAMPLE_HZ // ATTOSECONDS + 1
        print("\n".join(event_lines))
        print(next(line for line in lines if "C352_RESET_REQUEST" in line))
        print(next(line for line in lines if "C352_RESET_PCM" in line))
        print(f"reset_time={actual_reset[0]}:{actual_reset[1]}; predicted second onset sample={predicted}")
        if req_flags != 0x0006 or final_flags != 0x0006 or reset_count < 2:
            raise AssertionError(f"unexpected source status: request={req_flags:04x} final={final_flags:04x} events={reset_count}")
        if len(bursts) < 2 or bursts[0][0] != 1:
            raise AssertionError(f"expected the initial burst at sample 1 and a second burst, got {bursts}")
        second = bursts[1][0]
        if second < 1000:
            raise AssertionError(f"sample cursor appears to have restarted across soft reset: second burst at {second}")
        if second != predicted:
            raise AssertionError(f"second burst lost H8/stream phase: actual={second}, expected {predicted}")
        unit = 1.0 / 4096.0
        if bursts[0][1][:4] != [unit, 2 * unit, 3 * unit, 0.0]:
            raise AssertionError(f"unexpected first-burst PCM: {bursts[0]}")
        off = (reset_attos + (KEYON_CYCLE + 408) * ATTOS_PER_H8) * SAMPLE_HZ // ATTOSECONDS + 1
        count = off - predicted
        expected_second = [i * unit for i in range(1, count + 1)] + [0.0] * (5 - count)
        if bursts[1][1] != expected_second:
            raise AssertionError(f"unexpected second-burst PCM: {bursts[1]}, expected {expected_second}")
        print(f"MAME C352 reset oracle passed: {captured} frames, second burst at absolute sample {second}")
    finally:
        if args.keep:
            print(f"transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
