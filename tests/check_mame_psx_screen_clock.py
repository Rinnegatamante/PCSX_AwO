#!/usr/bin/env python3
"""Oracle MAME for PSX GPU GP1 display-mode and screen vblank timing."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "tests/mame_psx_screen_clock_oracle.lua"
ATTO = 10**18
CASES = {
    "initial": {"actions": [], "samples": [(0.005, "initial")]},
    "ntsc480": {"actions": [(0.010, "mode480", 0x08000004)],
                "samples": [(0.009, "before"), (0.010001, "after"), (0.025, "later")]},
    "pal50": {"actions": [(0.017, "pal50", 0x08000008)],
              "samples": [(0.016, "before"), (0.017001, "after"), (0.039, "later")]},
    # The 17 ms change is after the first vblank start, so configure runs while
    # blanking is active and should leave the existing end timer target intact.
    "during_blank": {"actions": [(0.017, "pal50_in_blank", 0x08000008)],
                     "samples": [(0.016, "before_start"), (0.017001, "after_change")]},
    # PAL at 17 ms makes the current frame longer.  At 35 ms, switching to
    # NTSC 480 shortens the period below the age of the retained start epoch.
    "short_period": {"actions": [(0.017, "pal50", 0x08000008),
                                  (0.035, "ntsc480_short", 0x08000004)],
                      "samples": [(0.034, "before_shortening"),
                                  (0.035001, "after_shortening")]},
    # Reset to GPU defaults at 17 ms, during the first VBLANK.  The reset
    # updates the start schedule using the retained last start and leaves the
    # already armed end timer target available for comparison.
    "gpu_reset": {"actions": [(0.010, "mode480", 0x08000004),
                               (0.017, "gpu_reset", 0x00000000)],
                  "samples": [(0.016, "before_reset"), (0.017001, "after_reset")]},
    "soft_reset": {"actions": [(0.010, "mode480", 0x08000004),
                                 (0.017, "machine_soft_reset", None, "soft_reset")],
                   "samples": [(0.016, "before_reset"), (0.017001, "after_reset")]},
}


def lua_value(value: object) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, str):
        return json.dumps(value)
    if isinstance(value, list):
        return "{" + ",".join(lua_value(item) for item in value) + "}"
    if isinstance(value, tuple):
        return lua_value(list(value))
    if isinstance(value, dict):
        return "{" + ",".join(f"{key}={lua_value(item)}" for key, item in value.items()) + "}"
    raise TypeError(type(value))


def run_case(mame: str, rompath: Path, case_name: str, case: dict, work: Path) -> dict:
    actions = []
    for row in case["actions"]:
        item = {"at": row[0], "label": row[1], "kind": row[3] if len(row) > 3 else "gp1"}
        if len(row) > 2 and row[2] is not None:
            item["word"] = row[2]
        actions.append(item)
    samples = [{"at": at, "label": label} for at, label in case["samples"]]
    script_text = TEMPLATE.read_text()
    script_text = script_text.replace("__ACTIONS__", lua_value(actions))
    script_text = script_text.replace("__SAMPLES__", lua_value(samples))
    if "__ACTIONS__" in script_text or "__SAMPLES__" in script_text:
        raise RuntimeError("Lua timeline substitution failed")
    script = work / f"screen-clock-{case_name}.lua"
    script.write_text(script_text)
    command = [mame, "soulclbrac", "-noreadconfig", "-rompath", str(rompath),
               "-skip_gameinfo", "-video", "none", "-sound", "none", "-nothrottle",
               "-seconds_to_run", "2", "-nonvram_save",
               "-nvram_directory", str(work / f"nvram-{case_name}"),
               "-cfg_directory", str(work / f"cfg-{case_name}"), "-inipath", str(work),
               "-autoboot_script", str(script), "-autoboot_delay", "0"]
    process = subprocess.Popen(command, cwd=work, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    timed_out = False
    try:
        output, _ = process.communicate(timeout=45)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.send_signal(signal.SIGTERM)
        try:
            output, _ = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate()

    writes = []
    for label, kind, word, bsec, battos, asec, aattos in re.findall(
            r"PSX_CLOCK_WRITE label=(\S+) kind=(\S+) word=(\S+) before=(\d+):(\d+) after=(\d+):(\d+)", output):
        writes.append({"label": label, "kind": kind,
                       "word": int(word, 16) if word != "-" else None,
                       "before": {"seconds": int(bsec), "attoseconds": int(battos)},
                       "after": {"seconds": int(asec), "attoseconds": int(aattos)}})
    observations = []
    for label, now, start, finish, period in re.findall(
            r"PSX_CLOCK_SAMPLE label=(\S+) now=(\d+:\d+) start=(\d+:\d+) finish=(\d+:\d+) period=([0-9.]+)", output):
        def parse_stamp(value: str) -> dict[str, int]:
            sec, attos = value.split(":")
            return {"seconds": int(sec), "attoseconds": int(attos)}
        period_float = float(period)
        observations.append({"label": label, "now": parse_stamp(now),
                             "start": parse_stamp(start), "finish": parse_stamp(finish),
                             "period_seconds": period_float,
                             "period_attoseconds_rounded": round(period_float * ATTO)})
    reset_events = []
    for n, sec, attos, mainpc, h8pc, start, finish in re.findall(
            r"PSX_CLOCK_RESET n=(\d+) now=(\d+):(\d+) mainpc=([0-9a-fA-F]+) h8pc=([0-9a-fA-F]+) start=(\d+:\d+) finish=(\d+:\d+)", output):
        def parse_reset_stamp(value: str) -> dict[str, int]:
            s, a = value.split(":")
            return {"seconds": int(s), "attoseconds": int(a)}
        reset_events.append({"n": int(n), "time": {"seconds": int(sec), "attoseconds": int(attos)},
                             "main_pc": int(mainpc, 16), "h8_pc": int(h8pc, 16),
                             "start": parse_reset_stamp(start), "finish": parse_reset_stamp(finish)})
    if process.returncode != 0 or timed_out or "LUA ERROR" in output:
        raise RuntimeError(f"MAME case {case_name} failed (return={process.returncode}, timeout={timed_out}):\n"
                           + "\n".join(output.splitlines()[-70:]))
    if len(observations) != len(samples) or len(writes) != len(actions):
        raise RuntimeError(f"MAME case {case_name} emitted incomplete evidence:\n"
                           + "\n".join(output.splitlines()[-70:]))
    return {"command": command, "writes": writes, "samples": observations,
            "reset_events": reset_events, "stdout": output}


def attos(stamp: dict[str, int]) -> int:
    return stamp["seconds"] * ATTO + stamp["attoseconds"]


def require(condition: bool, description: str, evidence: dict) -> None:
    if not condition:
        raise RuntimeError(f"screen clock invariant failed: {description}\n"
                           + json.dumps(evidence, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path, help="write combined JSON evidence to this path")
    parser.add_argument("--keep", action="store_true", help="retain transient files in /tmp")
    args = parser.parse_args()
    parent = args.rompath / "soulclbr.zip"
    if not parent.is_file():
        parser.error(f"requires parent ROM archive soulclbr.zip in {args.rompath}")
    version = subprocess.run([args.mame, "-version"], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=10, check=False)
    work = Path(tempfile.mkdtemp(prefix="mame-psx-screen-clock-"))
    rom_before = hashlib.sha256(parent.read_bytes()).hexdigest()
    fixture_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
    try:
        shutil.copyfile(parent, work / "soulclbrac.zip")
        evidence = {"mame_version": version.stdout.strip(),
                    "source_revision_pin": "deb2f54fe7300e7a192690318beb860159bfd52a",
                    "installed_binary_source_match": "unknown; locally installed MAME 0.287 prebuilt",
                    "rom_sha256_before": rom_before, "cases": {}}
        for name, case in CASES.items():
            row = run_case(args.mame, work, name, case, work)
            evidence["cases"][name] = row
            print(f"{name}: writes={len(row['writes'])} samples={len(row['samples'])}")
            for sample in row["samples"]:
                print(f"  {sample['label']}: now={sample['now']} start={sample['start']} "
                      f"finish={sample['finish']} period_as≈{sample['period_attoseconds_rounded']}")
        rows = evidence["cases"]
        initial = rows["initial"]["samples"][0]
        initial_start = 16_715_113_233_669_674
        initial_end = 19_215_113_233_669_674
        require(attos(initial["start"]) == initial_start,
                "GPU default first start target must expose the known initial period", evidence)
        require(attos(initial["finish"]) == initial_end,
                "GPU default initial vblank end target must be 2.5 ms after start", evidence)

        mode480 = rows["ntsc480"]["samples"][1]
        require(attos(mode480["finish"]) - attos(mode480["start"]) == 2_500_000_000_000_000,
                "mode change must preserve the 2.5 ms vblank interval", evidence)

        pal = rows["pal50"]["samples"][1]
        require(attos(pal["start"]) == 36_715_113_233_669_674,
                "GP1 PAL mode must schedule the next START 20 ms from the retained epoch", evidence)
        require(attos(pal["finish"]) == initial_end,
                "mode change during active blank must retain the already scheduled END", evidence)
        blank = rows["during_blank"]["samples"]
        require(attos(blank[1]["finish"]) == attos(blank[0]["finish"]),
                "configure during vblank must retain the old END target", evidence)

        short = rows["short_period"]
        short_write = short["writes"][1]
        short_sample = short["samples"][1]
        require(attos(short_sample["finish"]) ==
                    attos(short_write["before"]) + 2_500_000_000_000_000,
                "reduced period older than the current start epoch must run vblank_begin immediately", evidence)
        require(attos(short_sample["start"]) > attos(short_sample["finish"]),
                "immediate vblank_begin must schedule its next START after the current END", evidence)

        reset = rows["gpu_reset"]
        reset_samples = reset["samples"]
        reset_start_expected = attos(reset_samples[0]["start"]) + initial_start
        require(attos(reset_samples[1]["start"]) == reset_start_expected,
                "GPU reset must schedule default START from the retained last START", evidence)
        require(attos(reset_samples[1]["finish"]) == attos(reset_samples[0]["finish"]),
                "GPU reset must leave the previously scheduled END timer intact", evidence)
        soft_reset = rows["soft_reset"]
        require(len(soft_reset["reset_events"]) >= 2,
                "MAME soft reset must deliver a separate machine reset notifier", evidence)
        soft_reset_samples = soft_reset["samples"]
        require(attos(soft_reset_samples[1]["start"]) ==
                attos(soft_reset_samples[0]["start"]) + initial_start,
                "MAME soft reset must configure default START from retained screen phase", evidence)
        require(attos(soft_reset_samples[1]["finish"]) == attos(soft_reset_samples[0]["finish"]),
                "MAME soft reset must leave the already scheduled END target intact", evidence)
        evidence["rom_sha256_after"] = hashlib.sha256(parent.read_bytes()).hexdigest()
        evidence["fixture_sha256_before"] = fixture_before
        evidence["fixture_sha256_after"] = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        if evidence["rom_sha256_before"] != evidence["rom_sha256_after"]:
            raise RuntimeError("parent ROM archive changed during oracle run")
        if evidence["fixture_sha256_before"] != evidence["fixture_sha256_after"]:
            raise RuntimeError("Lua oracle fixture changed during oracle run")
        if args.output:
            output_path = args.output.resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(json.dumps(evidence, indent=2) + "\n")
            print(f"evidence JSON: {output_path}")
        print("MAME PSX screen clock oracle passed")
        return 0
    finally:
        if args.keep:
            print(f"temporary MAME files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
