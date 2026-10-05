#!/usr/bin/env python3
"""Capture a production SCI1 RTC read from real System 12 H8 firmware in MAME."""
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
TEMPLATE = ROOT / "tests/mame_system12_rtc_oracle.lua"
H8_HZ = 16_934_400
ATTOSECONDS = 10**18
MAX_RAW_OBSERVATIONS = 96
EXPECTED_RTC = [0x00, 0x00, 0x12, 0x07, 0x04, 0x10, 0x26]
SOURCE_REVISION_PIN = "deb2f54fe7300e7a192690318beb860159bfd52a"


def load_mame_helper():
    helper_path = ROOT / "tests/check_mame_h8_peripherals.py"
    spec = importlib.util.spec_from_file_location("mame_h8_peripherals", helper_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load existing MAME helper {helper_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_firmware() -> list[int]:
    """H8/3002 SCI1 clocked RTC receiver, with RDRF polling for each byte."""
    words: list[int] = []

    def mov_imm_to_abs(value: int, address: int) -> None:
        words.extend((0xF800 | value, 0x6A88, address))

    # Namco System 12 RTC CE = port B.5 AND port A.0.  The receiver's SCI1
    # synchronous clock is internal; BRR=7 leaves room to service every RDRF.
    for address, value in (
        (0xFFD4, 0x20),  # DDRB
        (0xFFD1, 0x01),  # DDRA
        (0xFFD3, 0x01),  # DRA: select RTC
        (0xFFD6, 0x20),  # DRB: enable RTC
        (0xFFB8, 0x80),  # SCI1 synchronous mode
        (0xFFB9, 0x07),  # slow enough to drain each receive byte
        (0xFFBA, 0x10),  # SCI1 receiver enabled, RX only
    ):
        mov_imm_to_abs(value, address)

    for index in range(8):
        poll_address = 0x100 + len(words) * 2
        words.extend((0x6A08, 0xFFBC))  # MOV.B @SSR1,R0L
        words.append(0x7368)  # BTST #6,R0L (RDRF)
        branch_address = 0x100 + len(words) * 2
        after_branch = branch_address + 2
        displacement = poll_address - after_branch
        if not -128 <= displacement <= 127:
            raise AssertionError("SCI1 status loop does not fit a short branch")
        words.append(0x4700 | (displacement & 0xFF))  # BEQ poll while RDRF is clear
        words.extend((0x6A08, 0xFFBD))  # MOV.B @RDR1,R0L; SSR write below clears RDRF
        words.extend((0x6AA8, 0x0008, index))  # MOV.B R0L,@(0x080000 + index)
        words.extend((0xF884, 0x6A88, 0xFFBC))  # clear RDRF after SSR1 read + RDR1

    words.append(0x0180)  # SLEEP after the eight captured bytes
    return words


FIRMWARE = make_firmware()


def expected_capture(registers: list[int]) -> list[int]:
    """Pack MAME's 52 LSB-first RTC bits plus held-data clocks into SCI bytes."""
    bits: list[int] = []
    for index, value in enumerate(registers):
        width = 4 if index == 3 else 8  # RTC4543 weekday occupies only four bits
        bits.extend((value >> bit) & 1 for bit in range(width))
    bits.extend([bits[-1]] * (64 - len(bits)))
    return [sum(bits[start + bit] << bit for bit in range(8))
            for start in range(0, 64, 8)]


def make_rom(words: list[int], path: Path) -> None:
    rom = bytearray([0xFF]) * 0x80000
    rom[0:4] = bytes((0, 0, 1, 0))
    for index, word in enumerate(words):
        offset = 0x100 + index * 2
        rom[offset] = (word >> 8) & 0xFF
        rom[offset + 1] = word & 0xFF
    path.write_bytes(rom)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame", default=os.environ.get("MAME", "/opt/homebrew/bin/mame"))
    parser.add_argument("--rompath", type=Path, default=ROOT / "roms")
    parser.add_argument("--output", type=Path, help="write structured JSON evidence here")
    parser.add_argument("--log", type=Path, help="write complete MAME output here")
    parser.add_argument("--keep", action="store_true", help="retain private transient files")
    args = parser.parse_args()

    archive = args.rompath / "soulclbr.zip"
    if not archive.is_file():
        parser.error(f"requires parent archive soulclbr.zip in {args.rompath}")
    if not Path(args.mame).is_file():
        parser.error(f"MAME executable not found: {args.mame}")

    helper = load_mame_helper()
    work = Path(tempfile.mkdtemp(prefix="mame-system12-rtc-"))
    try:
        rom_before = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_before = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        version = subprocess.run([args.mame, "-version"], text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=10, check=False)
        if version.returncode:
            raise RuntimeError("MAME version probe failed: " + version.stdout[-2000:])

        lua = TEMPLATE.read_text(encoding="utf-8").replace(
            "FIRMWARE", ", ".join(f"0x{word:04x}" for word in FIRMWARE))
        script = work / "system12-rtc.lua"
        script.write_text(lua, encoding="utf-8")
        run_dir = work / "run"
        run_dir.mkdir()
        output, returncode, command, timed_out = helper.run_mame(
            args.mame, args.rompath.resolve(), script, run_dir)
        if args.log:
            log_target = args.log.resolve()
            log_target.parent.mkdir(parents=True, exist_ok=True)
            log_target.write_text(output, encoding="utf-8")

        resets = [
            {"n": int(n), "time": {"seconds": int(sec), "attoseconds": int(attos)},
             "pc": pc}
            for n, sec, attos, pc in re.findall(
                r"H8RTC_RESET n=(\d+) time=(\d+):(\d+) pc=([^\s]+)", output)
        ]
        shared_observations = []
        for address, value, sec, attos, pc, curbit, data, clk, ce in re.findall(
                r"H8RTC_STORE address=([0-9a-fA-F]+) value=([0-9a-fA-F]+) "
                r"time=(\d+):(\d+) pc=([^\s]+) curbit=(\d+) data=(\d+) "
                r"clk=(\d+) ce=(\d+)", output):
            timestamp = {"seconds": int(sec), "attoseconds": int(attos)}
            delta = timestamp["seconds"] * ATTOSECONDS + timestamp["attoseconds"]
            shared_observations.append({
                "address": int(address, 16), "value": int(value, 16),
                "time": timestamp,
                "cycle_ceil": (delta * H8_HZ + ATTOSECONDS - 1) // ATTOSECONDS,
                "pc": pc,
                "rtc_wire_state": {"curbit": int(curbit), "data": int(data),
                                   "clk": int(clk), "ce": int(ce)},
            })
        raw_register_observations = [
            {"address": int(address, 16), "value": int(value, 16),
             "time": {"seconds": int(sec), "attoseconds": int(attos)}, "pc": pc}
            for address, value, sec, attos, pc in re.findall(
                r"H8RTC_REG address=([0-9a-fA-F]+) value=([0-9a-fA-F]+) "
                r"time=(\d+):(\d+) pc=([^\s]+)", output)
        ]
        seeded_match = re.search(r"H8RTC_SEED bytes=([0-9a-f,]+) item_size=(\d+) item_count=(\d+)", output)
        seeded_bytes = ([int(value, 16) for value in seeded_match.group(1).split(",")]
                        if seeded_match else None)
        rom_after = hashlib.sha256(archive.read_bytes()).hexdigest()
        lua_after = hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()
        evidence = {
            "schema": "pcsx-aow.mame-system12-rtc.v1",
            "mame_version": version.stdout.strip(),
            "source_revision_pin": SOURCE_REVISION_PIN,
            "source_pin": "MAME System 12 namcos12.cpp RTC/SCI1 wiring; rtc4543.cpp register layout",
            "rom_hashes": {"parent_rom_before": rom_before, "parent_rom_after": rom_after,
                           "lua_template_before": lua_before, "lua_template_after": lua_after},
            "clock": {"h8_hz": H8_HZ, "attoseconds_per_second": ATTOSECONDS,
                      "timestamp_source": "MAME machine.time() at shared RAM program-space writes",
                      "cycle_rule": "ceil((seconds*1e18+attoseconds)*H8_HZ/1e18) relative to H8 reset"},
            "rtc_seed": {"seeded_rtc_bytes": seeded_bytes,
                         "item_size": int(seeded_match.group(2)) if seeded_match else None,
                         "item_count": int(seeded_match.group(3)) if seeded_match else None,
                         "expected": EXPECTED_RTC},
            "firmware_words": [f"0x{word:04x}" for word in FIRMWARE],
            "firmware": {"reset_pc": "0x000100", "receiver": "SCI1 synchronous RX-only",
                         "sci1_smr": "0x80", "sci1_brr": "0x07", "sci1_scr": "0x10",
                         "poll": "read SSR1, BTST #6 RDRF, loop while clear, read RDR1, write SSR1=0x84",
                         "rtc_stream": "52 LSB-first data bits (weekday 4 bits), followed by 12 clocks holding the final data bit",
                         "capture_count": 8, "capture_address": "0x080000-0x080007",
                         "final_instruction": "SLEEP"},
            "runs": [{"returncode": returncode, "timed_out": timed_out,
                      "command": command}],
            "reset_events": resets,
            "raw_register_observations": raw_register_observations[:MAX_RAW_OBSERVATIONS],
            "shared_observations": shared_observations,
            "observation_window": {"raw_register_limit": MAX_RAW_OBSERVATIONS,
                                   "firmware_store_count": 8},
            "stdout_tail": output[-8000:],
        }
        if args.output:
            target = args.output.resolve()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
            print(f"evidence JSON: {target}")

        if returncode or timed_out:
            raise RuntimeError(f"MAME run failed (returncode={returncode}, timed_out={timed_out}):\n"
                               + output[-6000:])
        if "LUA ERROR" in output:
            raise RuntimeError("MAME reported a Lua error:\n" + output[-6000:])
        if seeded_bytes != EXPECTED_RTC:
            raise RuntimeError(f"RTC seed did not match: {seeded_bytes!r}\n{output[-4000:]}")
        if len(resets) != 1 or resets[0]["time"] != {"seconds": 0, "attoseconds": 0}:
            raise RuntimeError(f"expected one H8 reset at time zero; got {resets!r}\n{output[-4000:]}")
        if len(shared_observations) != 8:
            raise RuntimeError(f"expected eight firmware stores; got {len(shared_observations)}\n"
                               + output[-6000:])
        expected_addresses = list(range(0x080000, 0x080008))
        if [row["address"] for row in shared_observations] != expected_addresses:
            raise RuntimeError("firmware shared RAM stores are not contiguous/in order: "
                               + repr(shared_observations))
        captured = [row["value"] for row in shared_observations]
        stream_expected = expected_capture(EXPECTED_RTC)
        if captured != stream_expected:
            raise RuntimeError(f"captured serial RTC stream differs from MAME RTC bitstream: "
                               f"{captured!r} != {stream_expected!r}")
        if "H8RTC_CAPTURE_COMPLETE" not in output:
            raise RuntimeError("firmware did not reach its eight-byte completion point")
        if rom_before != rom_after or lua_before != lua_after:
            raise RuntimeError("parent ROM archive or Lua template changed during capture")

        print(f"MAME System 12 RTC oracle passed: {len(shared_observations)} bytes, "
              f"{len(raw_register_observations)} raw SCI reads")
        for row in shared_observations:
            print(f"RTC {row['address']:06x}={row['value']:02x} cycle~{row['cycle_ceil']}")
        return 0
    finally:
        if args.keep:
            print(f"private transient files retained at {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
