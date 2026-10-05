#!/usr/bin/env python3
"""Capture System 12's real PSX/H8 shared-mailbox scheduling in MAME."""
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
TEMPLATE = ROOT / "tests/mame_system12_mailbox_oracle.lua"
H8_HZ = 16_934_400
MAME_MAIN_HZ = 25_000_000
ATTOSECONDS = 10**18
SOURCE_REVISION_PIN = "deb2f54fe7300e7a192690318beb860159bfd52a"


def load_mame_helper():
    helper_path = ROOT / "tests/check_mame_h8_peripherals.py"
    spec = importlib.util.spec_from_file_location("mame_h8_peripherals", helper_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load MAME helper {helper_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


MAIN_FIRMWARE = [
    0x3C081F08, 0x34091122, 0xA5090000,
    0x950A0002, 0x00000000, 0x1140FFFD, 0x00000000,
    0x340B3344, 0x154BFFFA, 0x00000000, 0xA50A0004,
    0x0BF0000B, 0x00000000,
]


def make_h8_firmware() -> list[int]:
    # MOV.W @0x080000,R0; CMP.W #0x1122,R0; BNE poll; publish 0x3344;
    # SLEEP. The relative branch is computed from the following instruction.
    words = [0x6B20, 0x0008, 0x0000, 0x7920, 0x1122, 0x4600,
             0x7900, 0x3344, 0x6BA0, 0x0008, 0x0002, 0x0180]
    poll = 0x100
    branch = poll + 5 * 2
    displacement = poll - (branch + 2)
    if not -128 <= displacement <= 127:
        raise AssertionError("H8 polling branch does not fit")
    words[5] = 0x4600 | (displacement & 0xFF)
    return words


H8_FIRMWARE = make_h8_firmware()


def attoseconds(timestamp: dict[str, int]) -> int:
    return timestamp["seconds"] * ATTOSECONDS + timestamp["attoseconds"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path, help="write structured evidence JSON")
    parser.add_argument("--log", type=Path, help="write complete MAME output")
    parser.add_argument("--keep", action="store_true", help="retain transient run files")
    args = parser.parse_args()

    archive = args.rompath / "soulclbr.zip"
    if not archive.is_file():
        parser.error(f"requires parent ROM archive soulclbr.zip in {args.rompath}")
    if not Path(args.mame).is_file():
        parser.error(f"MAME executable not found: {args.mame}")

    helper = load_mame_helper()
    work = Path(tempfile.mkdtemp(prefix="mame-system12-mailbox-"))
    try:
        rom_before = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        version = subprocess.run([args.mame, "-version"], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=10, check=False)
        if version.returncode:
            raise RuntimeError("MAME version probe failed: " + version.stdout[-2000:])

        script = work / "system12-mailbox.lua"
        shutil.copyfile(TEMPLATE, script)
        run_dir = work / "run"
        run_dir.mkdir()
        output, returncode, command, timed_out = helper.run_mame(
            args.mame, args.rompath.resolve(), script, run_dir)
        if args.log:
            log_path = args.log.resolve()
            log_path.parent.mkdir(parents=True, exist_ok=True)
            log_path.write_text(output, encoding="utf-8")

        reset_events = [
            {"n": int(n), "time": {"seconds": int(sec), "attoseconds": int(attos)},
             "main_pc": pc_main, "sub_pc": pc_sub}
            for n, sec, attos, pc_main, pc_sub in re.findall(
                r"S12MBX_RESET n=(\d+) time=(\d+):(\d+) main_pc=([^\s]+) sub_pc=([^\s]+)", output)
        ]
        loaded_firmware = {cpu: [int(word, 16) for word in words.split(',')]
                           for cpu, words in re.findall(
                               r'S12MBX_FIRMWARE cpu=(main|sub) words=([0-9a-fA-F,]+)', output)}
        events = []
        event_pattern = re.compile(
            r"S12MBX_EVENT sequence=(\d+) cpu=(\w+) kind=(\w+) "
            r"address=([0-9a-fA-F]+) offset=([0-9a-fA-F]+) "
            r"value=([0-9a-fA-F]+) data=([0-9a-fA-F]+) mem_mask=([0-9a-fA-F]+) "
            r"time=(\d+):(\d+) cycles=(-?\d+) cycle_source=([^\s]+) pc=([0-9a-fA-F]+)")
        for match in event_pattern.finditer(output):
            (sequence, cpu, kind, address, offset, value, data, mem_mask,
             sec, attos, cycles, cycle_source, pc) = match.groups()
            timestamp = {"seconds": int(sec), "attoseconds": int(attos)}
            row = {
                "sequence": int(sequence), "cpu": cpu, "kind": kind,
                "address": int(address, 16), "offset": int(offset, 16),
                "value": int(value, 16), "data": int(data, 16),
                "mem_mask": int(mem_mask, 16), "time": timestamp,
                "mame_cycles": int(cycles), "cycle_source": cycle_source,
                "pc": int(pc, 16),
            }
            if cpu == "sub":
                delta = attoseconds(timestamp) - attoseconds(reset_events[0]["time"]) if reset_events else attoseconds(timestamp)
                row["cycle_ceil"] = (delta * H8_HZ + ATTOSECONDS - 1) // ATTOSECONDS
                row["cycle_ceil_hz"] = H8_HZ
            elif cpu == "main":
                delta = attoseconds(timestamp) - attoseconds(reset_events[0]["time"]) if reset_events else attoseconds(timestamp)
                row["scheduler_cycle_ceil"] = (delta * MAME_MAIN_HZ + ATTOSECONDS - 1) // ATTOSECONDS
                row["scheduler_cycle_ceil_hz"] = MAME_MAIN_HZ
            events.append(row)

        rom_after = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_after = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        evidence = {
            "schema": "pcsx-aow.mame-system12-mailbox.v1",
            "mame_version": version.stdout.strip(),
            "source_revision_pin": SOURCE_REVISION_PIN,
            "source_pin": "MAME System 12 namcos12.cpp shared RAM map and PSX/H8 clocks; CPU program-space taps",
            "rom_hashes": {"parent_rom_before": rom_before, "parent_rom_after": rom_after,
                           "lua_template_before": lua_before, "lua_template_after": lua_after},
            "clock": {
                "reset_epoch": "MAME machine reset notifier at time zero",
                "timestamp_source": "MAME machine.time() inside real program-space callbacks",
                "h8_hz": H8_HZ,
                "mame_main_scheduler_hz": MAME_MAIN_HZ,
                "comparison_rule": "compare exact reset-relative scheduler time; raw CPU counters use different clock units",
            },
            "firmware": {
                "main_words": [f"0x{word:08x}" for word in MAIN_FIRMWARE],
                "sub_words": [f"0x{word:04x}" for word in H8_FIRMWARE],
                "main_reset_pc": "0xbfc00000",
                "sub_reset_pc": "0x000100",
                "main_behavior": "SH 0x1122 at 0x1f080000; poll 0x1f080002 for 0x3344; SH ACK at 0x1f080004; park at 0xbfc0002c",
                "sub_behavior": "poll 0x080000 for 0x1122; MOV.W 0x3344 to 0x080002; SLEEP",
            },
            "firmware_words": {
                cpu: [f"0x{word:x}" for word in words]
                for cpu, words in loaded_firmware.items()
            },
            "firmware_readback_source": "actual MAME ROM region bytes after injection",
            "runs": [{"returncode": returncode, "timed_out": timed_out, "command": command}],
            "reset_events": reset_events,
            "events": events,
            "ack_complete": "S12MBX_ACK_COMPLETE" in output,
            "event_capture": "real CPU memory taps; shared reads compressed to value changes and last read before changes",
            "stdout_tail": output[-8000:],
        }
        if args.output:
            target = args.output.resolve()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
            print(f"evidence JSON: {target}")

        if returncode or timed_out:
            raise RuntimeError(f"MAME run failed (returncode={returncode}, timed_out={timed_out}):\n{output[-6000:]}")
        if "LUA ERROR" in output:
            raise RuntimeError("MAME reported a Lua error:\n" + output[-6000:])
        if loaded_firmware != {"main": MAIN_FIRMWARE, "sub": H8_FIRMWARE}:
            raise RuntimeError(f"executed firmware differs from host fixture: {loaded_firmware!r}")
        if len(reset_events) != 1 or reset_events[0]["time"] != {"seconds": 0, "attoseconds": 0}:
            raise RuntimeError(f"expected one reset at time zero, got {reset_events!r}\n{output[-4000:]}")
        if not evidence["ack_complete"]:
            raise RuntimeError("mailbox ACK was not observed before timeout/exit:\n" + output[-6000:])
        writes = [row for row in events if row["kind"] == "write"]
        required = [("main", 0x1F080000, 0x1122), ("sub", 0x080002, 0x3344),
                    ("main", 0x1F080004, 0x3344)]
        for cpu, address, value in required:
            if not any(row["cpu"] == cpu and row["address"] == address and row["value"] == value
                       for row in writes):
                raise RuntimeError(f"missing mailbox write {(cpu, hex(address), hex(value))}: {writes!r}")
        if rom_before != rom_after or lua_before != lua_after:
            raise RuntimeError("parent ROM archive or Lua template changed during capture")
        if "Watchdog timer expired" in output or "watchdog" in output.lower() and "expired" in output.lower():
            raise RuntimeError("MAME watchdog expired during the probe:\n" + output[-4000:])

        print(f"MAME System 12 mailbox oracle passed: {len(events)} ordered read/write observations")
        for row in events:
            print(f"{row['sequence']:02d} {row['cpu']:>4} {row['kind']:<22} "
                  f"{row['address']:08x}={row['value']:08x} t={row['time']['seconds']}:{row['time']['attoseconds']:018d}")
        return 0
    finally:
        if args.keep:
            print(f"private transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
