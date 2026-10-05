#!/usr/bin/env python3
"""Capture real H8 ADC/timer bus accesses from a transient MAME ROM fixture.

The injected H8 firmware starts the software ADC or timer0, then polls the
registers from H8 machine code. MAME program-space taps report the access data
and exact scheduler timestamp; no frame callback is used as a clock source.
"""
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
TEMPLATE = ROOT / "tests/mame_h8_peripherals_oracle.lua"
H8_HZ = 16_934_400
ATTOSECONDS = 10**18
CYCLE_GRID_P = ATTOSECONDS // H8_HZ
MAX_OBSERVATIONS = 96
BASELINE_REF = "dde07db7"
NATIVE_SLICES = (1, 1024, 10000)

# H8/300H big-endian words. ADC: software start, then repeatedly read ADCSR.
ADC_FIRMWARE = [0xF820, 0x6A88, 0xFFE8, 0x0000, 0x0000, 0x0000, 0x0000,
                0x6A08, 0xFFE8, 0x6AA8, 0x0008, 0x0000, 0x40F4]
# Timer0: TCR=/1, enable TSTR bit 0, then poll TSR and both TCNT bytes.
TIMER_FIRMWARE = [0xF800, 0x6A88, 0xFF64, 0xF801, 0x6A88, 0xFF60,
                  0x6A08, 0xFF67, 0x6AA8, 0x0008, 0x0000,
                  0x6A08, 0xFF68, 0x6AA8, 0x0008, 0x0001,
                  0x6A08, 0xFF69, 0x6AA8, 0x0008, 0x0002, 0x40E0]


def make_sci_firmware(scr: int = 0x30) -> list[int]:
    words: list[int] = []
    for address, value in ((0xFFB1, 0), (0xFFB2, scr), (0xFFB3, 0x42)):
        words.extend((0xF800 | value, 0x6A88, address))
    words.extend((0x6A08, 0xFFB4, 0xF804, 0x6A88, 0xFFB4))
    loop = 0x100 + len(words) * 2
    words.extend((0x6A08, 0xFFB4, 0x6AA8, 0x0008, 0x0000))
    after_branch = 0x100 + (len(words) + 1) * 2
    words.append(0x4000 | ((loop - after_branch) & 0xff))
    return words


SCI_FIRMWARE = make_sci_firmware()
SCI_EXTERNAL_FIRMWARE = make_sci_firmware(0x32)


def run_mame(mame: str, rompath: Path, script: Path, work: Path) -> tuple[str, int, list[str], bool]:
    command = [mame, "soulclbrac", "-noreadconfig", "-rompath", str(rompath),
               "-skip_gameinfo", "-video", "none", "-sound", "none", "-nothrottle",
               "-seconds_to_run", "2", "-nonvram_save",
               "-nvram_directory", str(work / "nvram"),
               "-cfg_directory", str(work / "cfg"), "-inipath", str(work),
               "-autoboot_script", str(script), "-autoboot_delay", "0"]
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
        return output, proc.returncode if proc.returncode is not None else -999, command, True
    return output, proc.returncode, command, False


def absolute_attos(t: dict[str, int]) -> int:
    return t["seconds"] * ATTOSECONDS + t["attoseconds"]


def split_attos(n: int) -> dict[str, int]:
    seconds, attoseconds = divmod(n, ATTOSECONDS)
    return {"seconds": seconds, "attoseconds": attoseconds}


def make_rom(words: list[int], path: Path) -> None:
    rom = bytearray([0xff]) * 0x80000
    rom[0:4] = bytes((0, 0, 1, 0))
    for index, word in enumerate(words):
        start = 0x100 + index * 2
        rom[start] = (word >> 8) & 0xff
        rom[start + 1] = word & 0xff
    path.write_bytes(rom)


def cycle_ceil(t: dict[str, int], epoch: dict[str, int]) -> int:
    delta = absolute_attos(t) - absolute_attos(epoch)
    return (delta * H8_HZ + ATTOSECONDS - 1) // ATTOSECONDS


def compile_historical_fixture(work: Path) -> Path:
    source = subprocess.run(["git", "show", BASELINE_REF + ":libpcsxcore/arcade/h8.cpp"],
                            cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=10, check=False)
    if source.returncode:
        raise RuntimeError("cannot read historical HEAD h8.cpp:\n" + source.stdout[-2000:])
    historical = work / "h8-head.cpp"
    historical.write_text(source.stdout)
    binary = work / "h8-mame-peripheral-baseline"
    core = ROOT / "libpcsxcore/arcade"
    compiler = os.environ.get("CXX", "clang++")
    command = [compiler, "-std=c++11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
               "-I", str(core), str(historical),
               str(ROOT / "tests/h8_mame_peripheral_fixture.cpp"), "-o", str(binary)]
    built = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=120, check=False)
    if built.returncode:
        raise RuntimeError("historical HEAD comparator build failed:\n" + built.stdout[-5000:])
    return binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path, help="write structured JSON evidence here")
    parser.add_argument("--keep", action="store_true", help="retain private transient files")
    parser.add_argument("--include-sci", action="store_true",
                        help="include the serial status firmware once the host pin model is integrated")
    args = parser.parse_args()
    firmware_by_mode = {"adc": ADC_FIRMWARE, "timer": TIMER_FIRMWARE,
                        "sci": SCI_FIRMWARE, "sci_external": SCI_EXTERNAL_FIRMWARE}
    active_modes = tuple(firmware_by_mode) if args.include_sci else ("adc", "timer")
    archive = args.rompath / "soulclbr.zip"
    if not archive.is_file():
        parser.error(f"requires parent archive soulclbr.zip in {args.rompath}")
    if not Path(args.mame).is_file():
        parser.error(f"MAME executable not found: {args.mame}")

    work = Path(tempfile.mkdtemp(prefix="mame-h8-peripherals-"))
    try:
        rom_before = hashlib.sha256(archive.read_bytes()).hexdigest()
        fixture_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        version = subprocess.run([args.mame, "-version"], text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=10, check=False)
        print("MAME " + version.stdout.strip())
        all_rows = []
        mame_observations = []
        commands = []
        run_meta = []
        raw_outputs = []
        for mode in active_modes:
            words = firmware_by_mode[mode]
            lua = TEMPLATE.read_text()
            lua = lua.replace("MODE", mode).replace(
                "FIRMWARE", ", ".join(f"0x{word:04x}" for word in words))
            lua = lua.replace("MAXOBS", str(MAX_OBSERVATIONS))
            script = work / f"h8-{mode}.lua"
            script.write_text(lua)
            (work / mode).mkdir()
            output, code, command, timed_out = run_mame(
                args.mame, args.rompath.resolve(), script, work / mode)
            commands.extend(command)
            run_meta.append({"mode": mode, "returncode": code, "timed_out": timed_out})
            raw_outputs.append(output)
            for match in re.finditer(
                    r"H8PERIPH mode=(\w+) kind=(read|write) reg=(\w+) value=([0-9a-fA-F]+) "
                    r"time=(\d+):(\d+) pc=([^\s]+)", output):
                row_mode, kind, reg, value, sec, attos, pc = match.groups()
                timestamp = {"seconds": int(sec), "attoseconds": int(attos)}
                all_rows.append({"mode": row_mode, "kind": kind, "register": reg,
                                 "value": int(value, 16), "time": timestamp,
                                 "pc": pc})
            for match in re.finditer(
                    r"H8OBS mode=(\w+) address=([0-9a-fA-F]+) value=([0-9a-fA-F]+) "
                    r"time=(\d+):(\d+) pc=([^\s]+)", output):
                row_mode, address, value, sec, attos, pc = match.groups()
                mame_observations.append({"mode": row_mode, "address": int(address, 16),
                                          "value": int(value, 16),
                                          "time": {"seconds": int(sec),
                                                   "attoseconds": int(attos)},
                                          "pc": pc})

        resets = []
        for out in raw_outputs:
            resets.extend({"mode": mode, "n": int(n),
                           "time": {"seconds": int(sec), "attoseconds": int(attos)},
                           "pc": pc}
                          for mode, n, sec, attos, pc in re.findall(
                              r"H8PERIPH_RESET mode=(\w+) n=(\d+) time=(\d+):(\d+) pc=([^\s]+)", out))
        for mode in active_modes:
            mode_resets = [r for r in resets if r["mode"] == mode]
            if len(mode_resets) != 1 or absolute_attos(mode_resets[0]["time"]) != 0:
                raise RuntimeError(f"{mode}: expected one firmware reset at cycle zero")
            observed = sum(r["mode"] == mode for r in mame_observations)
            if observed != MAX_OBSERVATIONS:
                raise RuntimeError(f"{mode}: incomplete observation window ({observed})")
            epoch = absolute_attos(mode_resets[0]["time"]) if mode_resets else 0
            for row in (r for r in all_rows if r["mode"] == mode):
                row["relative_to_reset"] = split_attos(absolute_attos(row["time"]) - epoch)
                delta = absolute_attos(row["time"]) - epoch
                row["cycle_ceil"] = (delta * H8_HZ + ATTOSECONDS - 1) // ATTOSECONDS
                row["cycle_phase_numerator"] = (delta * H8_HZ) % ATTOSECONDS
            for row in (r for r in mame_observations if r["mode"] == mode):
                row["relative_to_reset"] = split_attos(absolute_attos(row["time"]) - epoch)
                row["cycle_ceil"] = cycle_ceil(row["time"], mode_resets[0]["time"]
                                                if mode_resets else {"seconds": 0, "attoseconds": 0})
        flag_transitions = []
        adc_reads = [r for r in all_rows if r["mode"] == "adc"
                     and r["kind"] == "read" and r["register"] == "ADCSR"]
        for before, after in zip(adc_reads, adc_reads[1:]):
            if ((before["value"] ^ after["value"]) & 0x80) != 0:
                flag_transitions.append({"mode": "adc", "register": "ADCSR.ADF",
                                         "old": (before["value"] >> 7) & 1,
                                         "new": (after["value"] >> 7) & 1,
                                         "before_cycle_ceil": before["cycle_ceil"],
                                         "after_cycle_ceil": after["cycle_ceil"],
                                         "before_time": before["time"],
                                         "after_time": after["time"]})

        makefile = ROOT / "tests/Makefile.h8-peripherals"
        built = subprocess.run(["make", "-f", str(makefile), "BUILD=/tmp/h8-peripheral-tests", "all"], cwd=ROOT,
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=120, check=False)
        if built.returncode:
            raise RuntimeError("native comparator build failed:\n" + built.stdout[-5000:])
        native_observations = []
        historical_observations = []
        native_binary = Path("/tmp/h8-peripheral-tests/h8-mame-peripheral-fixture")
        historical_binary = compile_historical_fixture(work)
        for mode in active_modes:
            words = firmware_by_mode[mode]
            rom_path = work / f"native-{mode}.bin"
            make_rom(words, rom_path)
            expected = sum(row["mode"] == mode for row in mame_observations)
            for slice_cycles in NATIVE_SLICES:
                native = subprocess.run([str(native_binary), str(rom_path), str(expected), str(slice_cycles)], cwd=ROOT,
                                        text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=10, check=False)
                if native.returncode:
                    raise RuntimeError(f"native {mode}/{slice_cycles} comparator failed:\n{native.stdout[-3000:]}")
                for address, value, cycle in re.findall(
                        r"NATIVE_OBS address=([0-9a-fA-F]+) value=([0-9a-fA-F]+) cycle=(\d+)",
                        native.stdout):
                    native_observations.append({"mode": mode, "slice_cycles": slice_cycles,
                                                "address": int(address, 16),
                                                "value": int(value, 16), "cycle": int(cycle)})
            historical = subprocess.run([str(historical_binary), str(rom_path), str(expected)],
                                        cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=10, check=False)
            if historical.returncode:
                raise RuntimeError(f"historical HEAD {mode} comparator failed:\n"
                                   + historical.stdout[-3000:])
            for address, value, cycle in re.findall(
                    r"NATIVE_OBS address=([0-9a-fA-F]+) value=([0-9a-fA-F]+) cycle=(\d+)",
                    historical.stdout):
                historical_observations.append({"mode": mode, "address": int(address, 16),
                                                "value": int(value, 16), "cycle": int(cycle)})

        rom_after = hashlib.sha256(archive.read_bytes()).hexdigest()
        fixture_after = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        evidence = {
            "schema": "pcsx-aow.mame-h8-peripherals.v1",
            "mame_version": version.stdout.strip(),
            "source_revision_pin": "deb2f54fe7300e7a192690318beb860159bfd52a",
            "historical_baseline_ref": BASELINE_REF,
            "native_slice_cycles": NATIVE_SLICES,
            "installed_binary_source_match": "unknown; localMAME is a prebuilt binary",
            "runs": run_meta,
            "source_hashes": {"parent_rom_before": rom_before, "parent_rom_after": rom_after,
                              "lua_template_before": fixture_before,
                              "lua_template_after": fixture_after},
            "clock": {"h8_hz": H8_HZ, "attoseconds_per_second": ATTOSECONDS,
                      "cycle_grid_period_floor_attoseconds": CYCLE_GRID_P,
                      "timestamp_source": "MAME machine.time() at H8 program-space register taps"},
            "bus": {"space": "H8 program, big-endian", "mem_mask_rule":
                    "0xff00 selects high byte, otherwise low byte",
                    "firmware_reset_pc": "0x000100",
                    "firmware_words": {"adc": [f"0x{x:04x}" for x in ADC_FIRMWARE],
                    "timer": [f"0x{x:04x}" for x in TIMER_FIRMWARE],
                    "sci": [f"0x{x:04x}" for x in SCI_FIRMWARE],
                    "sci_external": [f"0x{x:04x}" for x in SCI_EXTERNAL_FIRMWARE]}},
            "reset_events": resets,
            "accesses": all_rows,
            "peripheral_flag_transitions": flag_transitions,
            "mame_observations": mame_observations,
            "observation_window": {"per_mode_limit": MAX_OBSERVATIONS,
                                   "policy": "first N external firmware trace stores per mode"},
            "native_observations": native_observations,
            "historical_head_observations": historical_observations,
            "stdout_tails": [out[-2000:] for out in raw_outputs],
        }
        if args.output:
            target = args.output.resolve()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(evidence, indent=2) + "\n")
            print(f"evidence JSON: {target}")
        for row in all_rows[:20]:
            print(f"{row['mode']} {row['kind']} {row['register']}={row['value']:02x} "
                  f"cycle~{row['cycle_ceil']}")
        timing_diffs = []
        value_diffs = []
        count_diffs = []
        historical_diffs = []
        for mode in active_modes:
            mame_rows = [r for r in mame_observations if r["mode"] == mode]
            for slice_cycles in NATIVE_SLICES:
                native_rows = [r for r in native_observations if r["mode"] == mode
                               and r["slice_cycles"] == slice_cycles]
                if len(mame_rows) != len(native_rows):
                    count_diffs.append({"mode": mode, "slice_cycles": slice_cycles,
                                        "mame_count": len(mame_rows), "native_count": len(native_rows)})
                count = min(len(mame_rows), len(native_rows))
                for index in range(count):
                    mrow, nrow = mame_rows[index], native_rows[index]
                    if (mrow["address"], mrow["value"]) != (nrow["address"], nrow["value"]):
                        value_diffs.append({"mode": mode, "slice_cycles": slice_cycles,
                                            "index": index,
                                            "mame": [mrow["address"], mrow["value"]],
                                            "native": [nrow["address"], nrow["value"]]})
                    mcycle = mrow["cycle_ceil"]
                    if mcycle != nrow["cycle"]:
                        timing_diffs.append({"mode": mode, "slice_cycles": slice_cycles,
                                             "index": index, "mame_cycle": mcycle,
                                             "native_cycle": nrow["cycle"],
                                             "delta": nrow["cycle"] - mcycle})
            old_rows = [r for r in historical_observations if r["mode"] == mode]
            old_count = min(len(mame_rows), len(old_rows))
            for index in range(old_count):
                mrow, old = mame_rows[index], old_rows[index]
                if ((mrow["address"], mrow["value"], mrow["cycle_ceil"])
                        != (old["address"], old["value"], old["cycle"])):
                    historical_diffs.append({"mode": mode, "index": index,
                                             "mame": [mrow["address"], mrow["value"],
                                                      mrow["cycle_ceil"]],
                                             "head": [old["address"], old["value"],
                                                      old["cycle"]]})
        evidence["comparison"] = {"compared_observations": len(native_observations),
                                  "first_value_difference": value_diffs[0] if value_diffs else None,
                                  "first_timing_difference": timing_diffs[0] if timing_diffs else None,
                                  "timing_difference_count": len(timing_diffs),
                                  "value_difference_count": len(value_diffs),
                                  "count_differences": count_diffs,
                                  "historical_head_difference_count": len(historical_diffs),
                                  "first_historical_head_difference":
                                      historical_diffs[0] if historical_diffs else None}
        if args.output:
            args.output.resolve().write_text(json.dumps(evidence, indent=2) + "\n")
        print("native comparison:", evidence["comparison"])
        if historical_diffs:
            print("first historical HEAD mismatch:", historical_diffs[0])
        if flag_transitions:
            print("first MAME ADC flag edge:", flag_transitions[0])
        if count_diffs or value_diffs or timing_diffs:
            raise RuntimeError("strict MAME/native comparison failed: "
                               + json.dumps(evidence["comparison"], sort_keys=True))
        if any(r["returncode"] for r in run_meta):
            raise RuntimeError("MAME returned nonzero:\n" + "\n".join(raw_outputs)[-5000:])
        if any(r["timed_out"] for r in run_meta):
            raise RuntimeError("MAME exceeded its 45-second watchdog")
        if any("LUA ERROR" in out for out in raw_outputs):
            raise RuntimeError("MAME reported a Lua error:\n" + "\n".join(raw_outputs)[-5000:])
        if not all(any(row["mode"] == mode and row["kind"] == "read" for row in all_rows)
                   for mode in active_modes):
            raise RuntimeError("firmware did not produce peripheral reads:\n" + "\n".join(raw_outputs)[-5000:])
        if rom_before != rom_after or fixture_before != fixture_after:
            raise RuntimeError("parent ROM archive or Lua fixture changed during capture")
        print(f"MAME H8 peripheral oracle passed: {len(all_rows)} bus accesses")
        return 0
    finally:
        if args.keep:
            print(f"private transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
