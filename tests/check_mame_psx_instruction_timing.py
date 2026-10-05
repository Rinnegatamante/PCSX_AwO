#!/usr/bin/env python3
"""Capture real MAME PSX instruction costs using shared-RAM store markers."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "tests/mame_psx_instruction_timing_oracle.lua"
MAIN_HZ = 25_000_000
ATTOSECONDS = 10**18
SOURCE_REVISION_PIN = "deb2f54fe7300e7a192690318beb860159bfd52a"
MARKERS = [dict(index=i, label=name, address=0x1F080000 + i * 2, value=0xA000 + i)
           for i, name in enumerate(("pre_mult", "post_mult_mflo", "pre_div",
                                     "post_div_mfhi_mflo", "pre_rtps", "post_rtps",
                                     "pre_branch", "post_branch_delay"))]


def load_mame_helper():
    helper_path = ROOT / "tests/check_mame_h8_peripherals.py"
    spec = importlib.util.spec_from_file_location("mame_h8_peripherals", helper_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load MAME helper {helper_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def assemble_firmware() -> list[int]:
    words = [
        0x3C081F08,                         # lui t0,0x1f08
        0x24090007, 0x240A0009,             # t1=7, t2=9
        0x3C0D4000, 0x408D6000, 0x00000000, 0x00000000,  # CU2 in Status; hazard gap
    ]
    for i in range(8):
        words.append(0x34000000 | ((16 + i) << 16) | (0xA000 + i))  # ori s{i},zero,marker
    words.extend([
        0xA5100000,                         # SH s0,0(t0): pre MULT
        0x012A0018, 0x00005812,             # MULT t1,t2; MFLO t3
        0xA5110002,                         # SH s1,2(t0): post MULT/MFLO
        0xA5120004,                         # SH s2,4(t0): pre DIV
        0x012A001A, 0x00005810, 0x00006012, # DIV t1,t2; MFHI t3; MFLO t4
        0xA5130006,                         # SH s3,6(t0): post DIV/MFHI/MFLO
        0xA5140008,                         # SH s4,8(t0): pre RTPS
        0x4A000001,                         # COP2 RTPS
        0xA515000A,                         # SH s5,10(t0): post RTPS
        0xA516000C,                         # SH s6,12(t0): pre branch
        0x11290002,                         # BEQ t1,t1,target, skipping one word after delay
        0x240E0055,                         # delay slot: addiu t6,zero,0x55
        0x240E0066,                         # skipped path
    ])
    target_index = len(words)
    words.append(0xA517000E)                # SH s7,14(t0): branch target
    park_index = len(words)
    words.extend([0x0BF00000 | park_index, 0])
    return words


MAIN_FIRMWARE = assemble_firmware()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    archive = args.rompath / "soulclbr.zip"
    if not archive.is_file():
        parser.error(f"requires parent ROM archive soulclbr.zip in {args.rompath}")
    if not Path(args.mame).is_file():
        parser.error(f"MAME executable not found: {args.mame}")

    helper = load_mame_helper()
    work = Path(tempfile.mkdtemp(prefix="mame-system12-psx-timing-"))
    try:
        rom_before = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        mame_binary_sha256 = hashlib.sha256(Path(args.mame).read_bytes()).hexdigest()
        version = subprocess.run([args.mame, "-version"], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=10, check=False)
        if version.returncode:
            raise RuntimeError("MAME version probe failed: " + version.stdout[-2000:])
        source = TEMPLATE.read_text(encoding="utf-8").replace(
            "MAIN_FIRMWARE", ", ".join(f"0x{word:08x}" for word in MAIN_FIRMWARE))
        script = work / "psx-instruction-timing.lua"
        script.write_text(source, encoding="utf-8")
        run_dir = work / "run"
        run_dir.mkdir()
        output, returncode, command, timed_out = helper.run_mame(
            args.mame, args.rompath.resolve(), script, run_dir)
        log_path = args.log.resolve()
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(output, encoding="utf-8")

        resets = [{"n": int(n), "time": {"seconds": int(sec), "attoseconds": int(attos)},
                   "pc": pc} for n, sec, attos, pc in re.findall(
                       r"S12PSXTIMING_RESET n=(\d+) time=(\d+):(\d+) pc=([^\s]+)", output)]
        loaded_match = re.search(r"S12PSXTIMING_FIRMWARE words=([0-9a-fA-F,]+)", output)
        loaded = [int(word, 16) for word in loaded_match.group(1).split(",")] if loaded_match else None
        stores = []
        pattern = re.compile(
            r"S12PSXTIMING_STORE sequence=(\d+) address=([0-9a-fA-F]+) value=([0-9a-fA-F]+) "
            r"width=(\d+) mem_mask=([0-9a-fA-F]+) time=(\d+):(\d+) pc=([0-9a-fA-F]+)")
        epoch = resets[0]["time"] if resets else {"seconds": 0, "attoseconds": 0}
        epoch_as = epoch["seconds"] * ATTOSECONDS + epoch["attoseconds"]
        for seq, addr, value, width, mask, sec, attos, pc in pattern.findall(output):
            time = {"seconds": int(sec), "attoseconds": int(attos)}
            delta = time["seconds"] * ATTOSECONDS + time["attoseconds"] - epoch_as
            stores.append({"sequence": int(seq), "address": int(addr, 16),
                           "value": int(value, 16), "width": int(width),
                           "mem_mask": int(mask, 16), "time": time,
                           "time_as": delta,
                           "cycle_ceil": (delta * MAIN_HZ + ATTOSECONDS - 1) // ATTOSECONDS,
                           "pc": int(pc, 16)})

        rom_after = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_after = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        evidence = {
            "schema": "pcsx-aow.mame-system12-psx-instruction-timing.v1",
            "mame_version": version.stdout.strip(),
            "mame_executable": {"path": str(Path(args.mame).resolve()),
                                "sha256": mame_binary_sha256,
                                "installed_binary_source_match": "unverified"},
            "source_revision_pin": SOURCE_REVISION_PIN,
            "source_pin": "MAME PSX CPU instruction execution and System 12 shared-RAM map",
            "rom_hashes": {"parent_rom_before": rom_before, "parent_rom_after": rom_after,
                           "lua_template_before": lua_before, "lua_template_after": lua_after},
            "clock": {"reset_epoch": "MAME machine reset notifier at time zero",
                      "timestamp_source": "machine.time() inside PSX program-space write tap",
                      "main_hz": MAIN_HZ, "attoseconds_per_second": ATTOSECONDS,
                      "comparison": "exact reset-relative attoseconds; no phase shift or tolerance"},
            "firmware_words": {"main": [f"0x{w:08x}" for w in MAIN_FIRMWARE],
                               "sub": ["0x0180"]},
            "loaded_main_firmware_words": [f"0x{w:08x}" for w in loaded] if loaded else None,
            "markers": MARKERS,
            "operations": [
                {"name": "MULT+MFLO", "before": 0, "after": 1},
                {"name": "DIV+MFHI/MFLO", "before": 2, "after": 3},
                {"name": "COP2 RTPS with Status.CU2 enabled", "before": 4, "after": 5},
                {"name": "taken BEQ plus delay-slot instruction", "before": 6, "after": 7},
            ],
            "runs": [{"returncode": returncode, "timed_out": timed_out, "command": command}],
            "reset_events": resets,
            "stores": stores,
            "complete": "S12PSXTIMING_COMPLETE" in output,
            "diagnostic": [line for line in output.splitlines()
                           if "S12PSXTIMING_DIAGNOSTIC" in line],
            "stdout_tail": output[-6000:],
        }
        args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
        args.output.resolve().write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        print(f"evidence JSON: {args.output.resolve()}")
        if returncode or timed_out:
            raise RuntimeError(f"MAME failed (returncode={returncode}, timed_out={timed_out}):\n{output[-6000:]}")
        if "LUA ERROR" in output:
            raise RuntimeError("MAME reported a Lua error:\n" + output[-6000:])
        if loaded != MAIN_FIRMWARE:
            raise RuntimeError(f"MAME ROM readback mismatch: {loaded!r}")
        if len(resets) != 1 or resets[0]["time"] != {"seconds": 0, "attoseconds": 0}:
            raise RuntimeError(f"expected exactly one reset at t=0, got {resets!r}")
        if not evidence["complete"] or len(stores) != 8:
            raise RuntimeError(f"expected eight marker stores before exit: {stores!r}\n{output[-4000:]}")
        expected = [(m["address"], m["value"]) for m in MARKERS]
        actual = [(r["address"], r["value"]) for r in stores]
        if actual != expected or any(row["width"] != 16 for row in stores):
            raise RuntimeError(f"marker sequence/width mismatch: {actual!r}")
        if rom_before != rom_after or lua_before != lua_after:
            raise RuntimeError("parent ROM archive or Lua source changed during capture")
        if "Watchdog timer expired" in output:
            raise RuntimeError("MAME watchdog expired during timing probe")
        print(f"MAME PSX instruction timing passed: {len(stores)} exact 16-bit markers")
        for marker, store in zip(MARKERS, stores):
            print(f"{marker['label']:<24} t={store['time_as']} as ({store['cycle_ceil']} grid cycles)")
        return 0
    finally:
        if args.keep:
            print(f"private transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
