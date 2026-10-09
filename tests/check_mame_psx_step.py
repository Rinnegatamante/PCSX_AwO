#!/usr/bin/env python3
"""Capture bounded real-MAME PSX branch/load/exception step behavior."""
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
TEMPLATE = ROOT / "tests/mame_psx_step_oracle.lua"
ROM_ARCHIVE = "soulclbr.zip"
ATTOSECONDS = 10**18
SOURCE_REVISION_PIN = "deb2f54fe7300e7a192690318beb860159bfd52a"
VECTOR_WORD = 0x180 // 4


def load_helper():
    path = ROOT / "tests/check_mame_h8_peripherals.py"
    spec = importlib.util.spec_from_file_location("mame_h8_peripherals", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load MAME helper {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ori(rt: int, value: int) -> int:
    return 0x34000000 | (rt << 16) | (value & 0xFFFF)


def addiu(rt: int, value: int) -> int:
    return 0x24000000 | (rt << 16) | (value & 0xFFFF)


def lui(rt: int, value: int) -> int:
    return 0x3C000000 | (rt << 16) | (value & 0xFFFF)


def sh(rt: int, offset: int, base: int = 8) -> int:
    return 0xA4000000 | (base << 21) | (rt << 16) | (offset & 0xFFFF)


def sw(rt: int, offset: int, base: int = 8) -> int:
    return 0xAC000000 | (base << 21) | (rt << 16) | (offset & 0xFFFF)


def lw(rt: int, offset: int, base: int = 8) -> int:
    return 0x8C000000 | (base << 21) | (rt << 16) | (offset & 0xFFFF)


def beq(rs: int, rt: int, immediate: int) -> int:
    return 0x10000000 | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


def park_jump(index: int) -> int:
    return 0x0BF00000 | index


def mfc0(rt: int, rd: int) -> int:
    return 0x40000000 | (rt << 16) | (rd << 11)


def addu(rd: int, rs: int, rt: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | 0x21


def finish(words: list[int]) -> list[int]:
    park = len(words)
    words.extend([park_jump(park), 0])
    return words


def exception_vector(park_index: int, completion_value: int) -> list[int]:
    # MFC0 has a load delay: each NOP lets the following SW see the CP0 value.
    return [mfc0(10, 13), 0, sw(10, 0x20),  # t2 = Cause; SW at shared +0x20
            mfc0(11, 14), 0, sw(11, 0x24),  # t3 = EPC; SW at shared +0x24
            ori(12, completion_value), sh(12, 0x26),
            park_jump(park_index), 0]


def make_cases() -> list[dict]:
    cases = []

    # Taken BEQ: its delay-slot SH runs, the sequential SH is skipped.
    words = [lui(8, 0x1F08), ori(9, 0xA101), ori(10, 0xA102),
             ori(11, 0xA1EE), ori(12, 0xA103), sh(9, 0), addiu(13, 1),
             beq(13, 13, 2), sh(10, 2), sh(11, 4), sh(12, 4)]
    words = finish(words)
    cases.append(dict(name="branch_taken", boot=words, vector=[], stop_address=0x1F080004,
                      stop_value=0xA103,
                      expectation={"stores": [[0x1F080000, 0xA101],
                                               [0x1F080002, 0xA102],
                                               [0x1F080004, 0xA103]],
                                   "delay_slot": "executed", "branch_target": "taken",
                                   "skipped_value_absent": 0xA1EE}))

    # Not-taken BEQ: delay slot then ordinary fallthrough, then distant target.
    words = [lui(8, 0x1F08), ori(9, 0xA111), ori(10, 0xA112), ori(11, 0xA113),
             ori(12, 0xA114), addiu(13, 1), addiu(14, 2), sh(9, 0),
             beq(13, 14, 2), sh(10, 2), sh(11, 4), sh(12, 6)]
    words = finish(words)
    cases.append(dict(name="branch_not_taken", boot=words, vector=[], stop_address=0x1F080006,
                      stop_value=0xA114,
                      expectation={"stores": [[0x1F080000, 0xA111],
                                               [0x1F080002, 0xA112],
                                               [0x1F080004, 0xA113],
                                               [0x1F080006, 0xA114]],
                                   "delay_slot": "executed", "branch_target": "not taken"}))

    # R3000 load delay: the immediately following SH should see old t2; the
    # next SH should see the fetched word. The source word is outside the taps.
    words = [lui(8, 0x1F08), ori(9, 0x2222), sw(9, 0x100), ori(10, 0x1111),
             lw(10, 0x100), sh(10, 0x10), sh(10, 0x12)]
    words = finish(words)
    cases.append(dict(name="load_delay", boot=words, vector=[], stop_address=0x1F080012,
                      stop_value=0x2222,
                      expectation={"stores": [[0x1F080010, 0x1111],
                                               [0x1F080012, 0x2222]],
                                   "load_delay": "immediate dependent SH uses old t2; next SH uses loaded value"}))

    # Syscall in a taken branch delay slot. Handler samples Cause/EPC then parks.
    boot = [lui(8, 0x1F08), ori(9, 0xA121), sh(9, 0), beq(0, 0, 1),
            0x0000000C, sh(9, 2)]
    park_index = len(boot)
    boot.extend([park_jump(park_index), 0])
    vector = exception_vector(park_index, 0xA122)
    cases.append(dict(name="exception_taken_delay", boot=boot, vector=vector,
                      stop_address=0x1F080026, stop_value=0xA122,
                      expectation={"cause_mask": 0x8000007C, "cause_value": 0x80000020,
                                   "epc": 0xBFC0000C, "bd": 1}))

    # The same exception in the slot of a not-taken branch probes BD/EPC rules.
    boot = [lui(8, 0x1F08), ori(9, 0xA131), addiu(10, 1), addiu(11, 2),
            sh(9, 0), beq(10, 11, 2), 0x0000000C, sh(9, 2), sh(9, 4)]
    park_index = len(boot)
    boot.extend([park_jump(park_index), 0])
    vector = exception_vector(park_index, 0xA132)
    cases.append(dict(name="exception_not_taken_delay", boot=boot, vector=vector,
                      stop_address=0x1F080026, stop_value=0xA132,
                      expectation={"cause_mask": 0x8000007C, "cause_value": 0x80000020,
                                   "epc": 0xBFC00014, "bd": 1}))

    # Branch-in-delay is architectureally unpredictable. Record actual MAME
    # path, bounded at the outer-branch target marker or two frame callbacks.
    words = [lui(8, 0x1F08), ori(9, 0xA141), ori(10, 0xA142),
             ori(11, 0xA143), sh(9, 0), beq(0, 0, 3),
             beq(0, 0, 1), sh(10, 2), sh(11, 4)]
    words = finish(words)
    cases.append(dict(name="branch_in_delay_exploratory", boot=words, vector=[],
                      stop_address=0x1F080004, stop_value=0xA143,
                      expectation={"architecture": "UNPREDICTABLE; preserve observed PC/store path"}))
    return cases


CASES = make_cases()


def full_main_image(case: dict) -> list[int]:
    words = list(case["boot"])
    if len(words) > VECTOR_WORD:
        raise AssertionError(f"{case['name']} boot code overlaps exception vector")
    words.extend([0] * (VECTOR_WORD - len(words)))
    words.extend(case["vector"])
    return words


def parse_run(output: str, case: dict, main_image: list[int], version: str,
              command: list[str], returncode: int, timed_out: bool) -> dict:
    resets = [{"n": int(n), "time": {"seconds": int(sec), "attoseconds": int(attos)}, "pc": pc}
              for name, n, sec, attos, pc in re.findall(
                  r"S12STEP_RESET case=([^\s]+) n=(\d+) time=(\d+):(\d+) pc=([0-9a-fA-F]+)", output)
              if name == case["name"]]
    firmware_match = re.search(
        r"S12STEP_FIRMWARE case=([^\s]+) main=([0-9a-fA-F,]*) vector=([0-9a-fA-F,]*) h8=([0-9a-fA-F]+)",
        output)
    # Keep parsing tolerant of a missing leading CASE token while preserving failure evidence.
    if firmware_match:
        _, main_hex, vector_hex, h8_hex = firmware_match.groups()
        loaded_main = [int(word, 16) for word in main_hex.split(",") if word]
        loaded_vector = [int(word, 16) for word in vector_hex.split(",") if word]
        loaded_h8 = int(h8_hex, 16)
    else:
        loaded_main, loaded_vector, loaded_h8 = None, None, None

    store_pattern = re.compile(
        r"S12STEP_STORE case=([^\s]+) sequence=(\d+) address=([0-9a-fA-F]+) "
        r"value=([0-9a-fA-F]+) width=(\d+) mem_mask=([0-9a-fA-F]+) "
        r"time=(\d+):(\d+) pc=([0-9a-fA-F]+)")
    stores = []
    epoch_as = 0
    for name, seq, addr, value, width, mask, sec, attos, pc in store_pattern.findall(output):
        if name != case["name"]:
            continue
        timestamp = {"seconds": int(sec), "attoseconds": int(attos)}
        time_as = timestamp["seconds"] * ATTOSECONDS + timestamp["attoseconds"] - epoch_as
        stores.append({"sequence": int(seq), "address": int(addr, 16),
                       "value": int(value, 16), "width": int(width),
                       "mem_mask": int(mask, 16), "time": timestamp,
                       "time_as": time_as, "pc": int(pc, 16)})
    complete = f"S12STEP_COMPLETE case={case['name']}" in output
    diagnostics = [line for line in output.splitlines()
                   if line.startswith(f"S12STEP_DIAGNOSTIC case={case['name']}")]
    return {"name": case["name"], "expectation": case["expectation"],
            "firmware_words": {"main": [f"0x{word:08x}" for word in main_image],
                               "boot": [f"0x{word:08x}" for word in case["boot"]],
                               "exception_vector": [f"0x{word:08x}" for word in case["vector"]],
                               "h8": ["0x0180"]},
            "loaded_firmware_readback": {"main_image": loaded_main,
                                         "exception_vector": loaded_vector,
                                         "h8_sleep": loaded_h8},
            "reset_events": resets, "stores": stores, "complete": complete,
            "diagnostic": diagnostics,
            "run": {"returncode": returncode, "timed_out": timed_out, "command": command},
            "stdout_tail": output[-5000:]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    archive = args.rompath / ROM_ARCHIVE
    if not archive.is_file():
        parser.error(f"requires parent ROM archive {ROM_ARCHIVE} in {args.rompath}")
    if not Path(args.mame).is_file():
        parser.error(f"MAME executable not found: {args.mame}")

    helper = load_helper()
    work = Path(tempfile.mkdtemp(prefix="mame-psx-step-"))
    try:
        rom_hash_before = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_hash_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        mame_path = Path(args.mame).resolve()
        mame_hash = hashlib.sha256(mame_path.read_bytes()).hexdigest()
        version_result = subprocess.run([str(mame_path), "-version"], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=10, check=False)
        if version_result.returncode:
            raise RuntimeError("MAME version probe failed: " + version_result.stdout[-2000:])

        cases = []
        logs = []
        for case in CASES:
            main_image = full_main_image(case)
            lua = TEMPLATE.read_text(encoding="utf-8")
            lua = lua.replace("@CASE_NAME@", case["name"])
            lua = lua.replace("@STOP_ADDRESS@", f"{case['stop_address']:08x}")
            lua = lua.replace("@STOP_VALUE@", f"{case['stop_value']:08x}")
            lua = lua.replace("@MAIN_FIRMWARE@", ", ".join(f"0x{word:08x}" for word in main_image))
            lua = lua.replace("@VECTOR_FIRMWARE@", ", ".join(f"0x{word:08x}" for word in case["vector"]))
            script = work / f"{case['name']}.lua"
            script.write_text(lua, encoding="utf-8")
            run_dir = work / case["name"]
            run_dir.mkdir()
            output, returncode, command, timed_out = helper.run_mame(
                str(mame_path), args.rompath.resolve(), script, run_dir)
            evidence_case = parse_run(output, case, main_image, version_result.stdout.strip(),
                                      command, returncode, timed_out)
            expected_readback = {"main_image": main_image,
                                 "exception_vector": case["vector"], "h8_sleep": 0x0180}
            readback_matches = evidence_case["loaded_firmware_readback"] == expected_readback
            evidence_case["firmware_readback_matches"] = readback_matches
            cases.append(evidence_case)
            logs.append(f"=== {case['name']} ===\n{output}\n")

        rom_hash_after = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_hash_after = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        evidence = {"schema": "pcsx-aow.mame-psx-step.v1",
                    "mame_version": version_result.stdout.strip(),
                    "mame_executable": {"path": str(mame_path), "sha256": mame_hash,
                                        "installed_binary_source_match": "unverified"},
                    "source_revision_pin": SOURCE_REVISION_PIN,
                    "rom_hashes": {"parent_rom_before": rom_hash_before,
                                   "parent_rom_after": rom_hash_after,
                                   "lua_template_before": lua_hash_before,
                                   "lua_template_after": lua_hash_after},
                    "clock": {"reset_epoch": "MAME machine reset notifier; every case resets at t=0",
                              "timestamp_source": "machine.time() inside PSX shared-RAM write tap",
                              "attoseconds_per_second": ATTOSECONDS},
                    "cases": cases}
        output_path = args.output.resolve()
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
        log_path = args.log.resolve()
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("\n".join(logs), encoding="utf-8")
        print(f"evidence JSON: {output_path}")

        failed = []
        for case in cases:
            name = case["name"]
            if case["run"]["returncode"] or case["run"]["timed_out"]:
                failed.append(f"{name}: MAME run failed/timed out")
            if not case["firmware_readback_matches"]:
                failed.append(f"{name}: injected ROM readback differs")
            if len(case["reset_events"]) != 1 or case["reset_events"][0]["time"] != {
                    "seconds": 0, "attoseconds": 0}:
                failed.append(f"{name}: reset epoch was not exactly zero")
            if "LUA ERROR" in case["stdout_tail"] or "Watchdog timer expired" in case["stdout_tail"]:
                failed.append(f"{name}: Lua error or watchdog expiration")
            if name != "branch_in_delay_exploratory" and not case["complete"]:
                failed.append(f"{name}: expected terminal marker missing")
        if rom_hash_before != rom_hash_after or lua_hash_before != lua_hash_after:
            failed.append("parent ROM archive or Lua template changed")

        for case in cases:
            print(f"{case['name']}: stores={len(case['stores'])} complete={case['complete']} "
                  f"firmware_readback={case['firmware_readback_matches']}")
            for store in case["stores"]:
                print(f"  pc={store['pc']:08x} {store['address']:08x}="
                      f"{store['value']:08x}/{store['width']} t={store['time_as']} as")
        if failed:
            raise RuntimeError("MAME source-step probe failed: " + "; ".join(failed))
        print(f"MAME PSX step oracle captured {len(cases)} cases")
        return 0
    finally:
        if args.keep:
            print(f"private transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
