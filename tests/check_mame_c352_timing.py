#!/usr/bin/env python3
"""Run a transient synthetic H8/C352 timing trace on installed MAME.

No ROM archive is modified. MAME writes use memory-region callbacks after the
real Soul Calibur set has loaded, and each run gets private NVRAM/config dirs.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "tests/mame_c352_timing_oracle.lua"
# Reset vector/opcode fetch = 6 H8 cycles. Each pre-keyon register transaction
# (MOV.W immediate, then absolute store) costs 12 cycles, and the final key-on
# transaction costs 12 more. The target offset is padded with 2-cycle NOPs.
KEYON_BASE_CYCLES = 78


def run_one(mame: str, rompath: Path, target_cycles: int, work: Path) -> tuple[int, int, int, list[tuple[float, float]]]:
    if target_cycles < KEYON_BASE_CYCLES or (target_cycles - KEYON_BASE_CYCLES) % 2:
        raise ValueError(f"target cycle {target_cycles} cannot be reached by whole H8 NOPs")
    delay_nops = (target_cycles - KEYON_BASE_CYCLES) // 2
    lua = TEMPLATE.read_text().replace("__KEYON_DELAY_NOPS__", str(delay_nops))
    if "__KEYON_DELAY_NOPS__" in lua:
        raise RuntimeError("Lua key-on delay substitution failed")
    script = work / f"c352-{target_cycles}.lua"
    script.write_text(lua)
    nvram = work / f"nvram-{target_cycles}"
    cfg = work / f"cfg-{target_cycles}"
    cmd = [mame, "soulclbrac", "-rompath", str(rompath), "-skip_gameinfo",
           "-video", "none", "-sound", "none", "-seconds_to_run", "3",
           "-nonvram_save", "-nvram_directory", str(nvram),
           "-cfg_directory", str(cfg), "-inipath", str(work),
           "-autoboot_script", str(script)]
    result = subprocess.run(cmd, cwd=work, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=45, check=False)
    match = re.search(rf"C352_ORACLE target={target_cycles} samples=(\d+) first_on=(-?\d+) first_off=(-?\d+) flags=([0-9a-fA-F]+) pcm=([^\r\n]+)", result.stdout)
    if result.returncode != 0 or not match:
        tail = "\n".join(result.stdout.splitlines()[-50:])
        raise RuntimeError(f"MAME oracle failed for cycle {target_cycles} (status {result.returncode}):\n{tail}")
    sample_count, first_on, first_off = map(int, match.group(1, 2, 3))
    pcm = [tuple(map(float, pair.split(":"))) for pair in match.group(5).split(",")]
    print(f"{target_cycles}: captured={sample_count}, first_nonzero={first_on}, first_silent_after={first_off}, flags=0x{match.group(4)}")
    print(f"  PCM[0:8]={pcm[:8]}")
    return first_on, first_off, int(match.group(4), 16), pcm


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mame", default=os.environ.get("MAME", shutil.which("mame") or "mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--keep", action="store_true", help="keep transient scripts/config under a temp directory")
    args = parser.parse_args()
    if not (args.rompath / "soulclbr.zip").exists():
        parser.error(f"requires local parent ROM archive soulclbr.zip in {args.rompath}")
    work = Path(tempfile.mkdtemp(prefix="c352-mame-oracle-"))
    version = subprocess.run([args.mame, "-version"], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=10, check=False)
    print(f"MAME {version.stdout.strip()}")
    observed = {}
    try:
        for edge in (190, 192, 194):
            observed[edge] = run_one(args.mame, args.rompath.resolve(), edge, work)
        # The stream update renders the current sample index inclusively before
        # each H8 register write. Thus 190 and 192-cycle key-on writes first
        # affect frame 1; 194 affects frame 2. The 192-NOP key-off then exposes
        # the corresponding source sample boundary in the second tuple item.
        assert observed[190][0] == 1, observed
        assert observed[192][0] == 1, observed
        assert observed[194][0] == 2, observed
        assert observed[190][1] == 4, observed
        assert observed[192][1] == 4, observed
        assert observed[194][1] == 4, observed
        assert all(row[2] == 0x0006 for row in observed.values()), observed
        unit = 1.0 / 4096.0
        expected = {
            190: [(0, 0), (unit, unit), (2 * unit, 2 * unit), (3 * unit, 3 * unit), (0, 0)],
            192: [(0, 0), (unit, unit), (2 * unit, 2 * unit), (3 * unit, 3 * unit), (0, 0)],
            194: [(0, 0), (0, 0), (unit, unit), (2 * unit, 2 * unit), (0, 0)],
        }
        for edge, expected_prefix in expected.items():
            assert observed[edge][3][:5] == expected_prefix, (edge, observed[edge][3][:5])
    finally:
        if args.keep:
            print(f"transient MAME scripts retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("MAME C352 timing oracle passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
