#!/usr/bin/env python3
"""Run synthetic Taito FX-1S media through the native headless harness.

The generated ROM regions are test programs and arbitrary CAT702/NVRAM bytes,
not game media or protection keys. This validates adapter wiring only.
"""
from __future__ import annotations

import argparse
import csv
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tests"))
import prepare_taito_fx1_board as prepare
from check_system10_harness import (_addiu, _i, _j, _jalr, _lui, _lw, _ori, _sb,
                                    _sh, _sw)

PROFILES = {row["set"]: row for row in json.loads(
    (ROOT / "scripts/taito_fx1_boards.json").read_text(encoding="utf-8"))["profiles"]}
SET_NAME = "sfchamp"


def _bne(rs: int, rt: int, immediate: int) -> int:
    return _i(5, rs, rt, immediate)


def _andi(rt: int, rs: int, immediate: int) -> int:
    return _i(0x0c, rs, rt, immediate)


def _lbu(rt: int, base: int, offset: int) -> int:
    return _i(0x24, base, rt, offset)


def main_program(cached: bool = False) -> bytes:
    code: list[int] = []
    # Bank 1 at 1fb40000 selects the helper at byte offset 8 MiB. The bit 5
    # transition is the MB3773 CK input on this Taito board.
    code += [_lui(8, 0x1fb4), _addiu(9, 0, 0x21), _sb(9, 8, 0)]
    code += [_lui(10, 0x9f00 if cached else 0xbf00),
             _ori(10, 10, 0x0100), _jalr(10), 0]

    # Verify upper RAM is physically independent from its low 2 MiB alias.
    code += [_lui(8, 0x8021), _lui(9, 0x51a7), _ori(9, 9, 0xcafe),
             _sw(9, 8, 0x10), _lw(20, 8, 0x10), 0,
             _lui(8, 0x8001), _lw(21, 8, 0x10), 0]

    # GP0 fill and readback through the 1 MiB/512-row GPU configuration.
    code += [_lui(8, 0xbf80), _ori(8, 8, 0x1810),
             _lui(9, 0x0200), _ori(9, 9, 0x00ff), _sw(9, 8, 0),
             _sw(0, 8, 0), _lui(9, 1), _ori(9, 9, 0x10), _sw(9, 8, 0),
             _lui(9, 0xc000), _sw(9, 8, 0), _sw(0, 8, 0),
             _lui(9, 1), _ori(9, 9, 2), _sw(9, 8, 0),
             _lw(19, 8, 0), 0]

    # Parallel EEPROM programming: immediately reads D7=1 (5a XOR 80), then
    # after >200 us of emulated PSX cycles it reads the programmed byte.
    code += [_lui(8, 0x1faf), _addiu(9, 0, 0x5a), _sb(9, 8, 1),
             _lbu(28, 8, 1)]

    # SIO0 drives the real ZN MCU and two CAT702 line mergers; arbitrary keys
    # are only fixture bytes. MCU digital reply for these DSW/input values is
    # the source-protocol header 1f.
    code += [_lui(8, 0x1fa1), _addiu(9, 0, 0x8c), _sb(9, 8, 0x300),
             _addiu(9, 0, 10000)]
    mcu_delay = len(code)
    code += [_addiu(9, 9, -1), _bne(9, 0, -2), 0]
    assert mcu_delay + 3 == len(code)
    code += [
             _lui(8, 0x1f80), _addiu(9, 0, 1), _sh(9, 8, 0x1048),
             _addiu(9, 0, 100), _sh(9, 8, 0x104e),
             _lui(9, 0), _ori(9, 9, 0x0c01), _sh(9, 8, 0x104a),
             _addiu(9, 0, 0x8c), _sb(9, 8, 0x1040)]
    serial_poll = len(code)
    code += [_lw(9, 8, 0x1044), 0, _andi(9, 9, 2),
             _bne(9, 0, 2), 0,
             # If RX is not ready, branch around the read and back to poll.
             _j(0), 0]
    # Patch final unconditional J back to poll. The `bne` above is not the
    # right polarity for waiting, so replace it with BEQ to zero.
    code[serial_poll + 3] = _i(4, 9, 0, -4)
    code[serial_poll + 5] = 0
    code += [_lbu(22, 8, 0x1040),
             _lw(24, 8, 0x1070), 0]

    # TC0140SYT master sends nibbles a,b to the Z80 and polls status mode 4
    # until the NMI routine has returned 6,b. The two reads clear the reply
    # full flag, which is checked again below.
    code += [_lui(8, 0x1fb8), _addiu(9, 0, 0), _sb(9, 8, 0),
             _addiu(9, 0, 0xa), _sb(9, 8, 2),
             _addiu(9, 0, 1), _sb(9, 8, 0),
             _addiu(9, 0, 0xb), _sb(9, 8, 2),
             _addiu(9, 0, 4), _sb(9, 8, 0)]
    comm_poll = len(code)
    code += [_lbu(10, 8, 2), 0, _andi(10, 10, 4),
             _i(4, 10, 0, 0), 0]
    code[-2] = _i(4, 10, 0, -4)
    code += [_addiu(27, 0, 0xa5),
             _addiu(9, 0, 0), _sb(9, 8, 0), _lbu(25, 8, 2),
             _addiu(9, 0, 1), _sb(9, 8, 0), _lbu(26, 8, 2),
             _addiu(9, 0, 4), _sb(9, 8, 0), _lbu(27, 8, 2)]

    # Wait longer than the exact EEPROM program deadline and sample it again.
    code += [_addiu(9, 0, 10000)]
    delay = len(code)
    code += [_addiu(9, 9, -1), _bne(9, 0, -2), 0]
    assert delay + 3 == len(code)
    code += [_lui(8, 0x1faf), _lbu(29, 8, 1)]

    # Poll Taito's profile-bound P1 input forever so frame 0/1 CSV events are
    # visible in the trace. Button 1 is bit 4, active low, in this input set.
    code += [_lui(8, 0x1fa0)]
    input_loop = len(code)
    code += [_lbu(14, 8, 0), _j(0), 0]
    code[-2] = 0x08000000 | (((0xbfc00000 + input_loop * 4) >> 2) & 0x03ffffff)
    return struct.pack(f"<{len(code)}I", *code)


def z80_program() -> bytes:
    rom = bytearray([0xff]) * 0x20000
    rom[:3] = b"\xc3\x00\x01"  # JP 0100
    # TC0140SYT NMI: read incoming command a,b in modes 0/1, store them,
    # then return reply 6,b to the master. Reading mode 1 acknowledges NMI.
    nmi = bytes.fromhex(
        "3e03 32 00f2 "  # diagnostic bank select, proving NMI entry in report
        "3e00 32 00e2 3a 01e2 32 00c1 "
        "3e01 32 00e2 3a 01e2 32 01c1 "
        "3e00 32 00e2 3e06 32 01e2 "
        "3e01 32 00e2 3e0b 32 01e2 ed45")
    rom[0x66:0x66 + len(nmi)] = nmi
    code = bytearray(bytes.fromhex("31 ff cf"))  # stack in source-mapped sound RAM
    # Enable TC0140 NMI, then configure YM2610B's real SSG tone channel.
    code.extend(bytes.fromhex("3e06 32 00e2 3e00 32 01e2"))  # enable NMI in mode 6
    for register, value in ((0x00, 1), (0x01, 0), (0x07, 0x3e),
                            (0x08, 0x0f), (0x24, 0xff), (0x25, 3), (0x27, 5)):
        code.extend((0x3e, register, 0x32, 0x00, 0xe0,
                     0x3e, value, 0x32, 0x01, 0xe0))
    code.extend((0x18, 0xfe))  # JR $; YM clocks continue
    rom[0x100:0x100 + len(code)] = code
    return bytes(rom)


def make_media(directory: Path, cached: bool = False) -> None:
    profile = PROFILES[SET_NAME]
    tags = {"maincpu:rom", "bankedroms", "audiocpu", "ym2610b:adpcma",
            "cat702_1", "cat702_2"}
    regions = {row["tag"]: bytearray([row["erase"]]) * row["size"]
               for row in profile["rom_regions"] if row["tag"] in tags}
    if set(regions) != tags:
        raise RuntimeError("selected source profile has unexpected FX1S region geometry")
    code = main_program(cached)
    regions["maincpu:rom"][:len(code)] = code
    # Bank selector 1 exposes offset 8 MiB at 1f000000/BF000000.
    helper = struct.pack("<4I", _addiu(17, 0, 0x5a), 0x03e00008, 0, 0)
    regions["bankedroms"][0x800100:0x800100 + len(helper)] = helper
    regions["audiocpu"][:] = z80_program()
    # The YM2610B SSG tone does not consume ADPCM; leave the actual sample
    # region erased as the source profile specifies.
    prepare.package_regions(SET_NAME, profile,
                            {name: bytes(data) for name, data in regions.items()},
                            directory,
                            {"synthetic_fixture": True,
                             "rom_source": "generated MIPS/Z80 test programs",
                             "keys": "arbitrary synthetic bytes; no protection claim"})


def run(harness: Path, directory: Path, *args: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([str(harness), "--taito-fx1", str(directory), *args],
                            capture_output=True, text=True, timeout=120)
    if result.stderr:
        print(result.stderr, end="")
    return result


def _trace_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as file:
        return list(csv.DictReader(file))


def check_guest(result: subprocess.CompletedProcess[str], trace_path: Path,
                audio_path: Path) -> None:
    result.check_returncode()
    for marker in ("set=sfchamp", "ram=4MiB vram=1MiB",
                   "source_cpu=CXD8530CQ", "fault=0"):
        if marker not in result.stderr:
            raise RuntimeError(f"FX-1S harness run missing diagnostic {marker!r}")
    rows = _trace_rows(trace_path)
    if not rows or rows[0]["pc"] != "bfc00000":
        raise RuntimeError("guest did not start at BFC00000")
    completed = [row for row in rows if row["r29"] == "0000005a"]
    if not completed:
        raise RuntimeError("MIPS guest never completed the TC0140 exchange and EEPROM read")
    expected = {17: 0x5a, 19: 0x001f001f, 20: 0x51a7cafe, 21: 0,
                22: 0x1f, 25: 6, 26: 0xb, 27: 0,
                28: 0xda, 29: 0x5a}
    for reg, value in expected.items():
        if not all(row[f"r{reg}"] == f"{value:08x}" for row in completed):
            raise RuntimeError(f"MIPS FX-1S fixture did not verify r{reg}={value:08x}")
    if not all(int(row["r24"], 16) & 0x80 for row in completed):
        raise RuntimeError("SIO0 serial transfer did not latch PSX IRQ7")
    for value in (0xef, 0xff):
        if not any(row["r14"] == f"{value:08x}" for row in rows):
            raise RuntimeError(f"frame-indexed P1 button input value {value:02x} missing")
    with wave.open(str(audio_path), "rb") as wav:
        if wav.getnchannels() != 2 or wav.getsampwidth() != 2 or wav.getframerate() != 55555:
            raise RuntimeError("invalid Taito FX-1S stereo PCM format")
        pcm = wav.readframes(wav.getnframes())
        if wav.getnframes() < 100 or not any(pcm):
            raise RuntimeError("executed Z80/YM2610B SSG program produced no PCM")


def reject_before_guest(harness: Path, valid: Path, directory: Path,
                        mutation: str, truncate_file: str | None = None) -> None:
    shutil.copytree(valid, directory)
    if mutation == "set":
        config = (directory / "board.ini").read_text(encoding="utf-8")
        (directory / "board.ini").write_text(config.replace("mame_set=sfchamp\n", "mame_set=ftimpct\n"),
                                               encoding="utf-8")
    elif mutation == "geometry":
        config = (directory / "board.ini").read_text(encoding="utf-8")
        (directory / "board.ini").write_text(config.replace("vram_size=1048576", "vram_size=2097152"),
                                               encoding="utf-8")
    elif mutation == "duplicate":
        with (directory / "board.ini").open("a", encoding="utf-8") as config:
            config.write("mame_set=sfchamp\n")
    if truncate_file:
        (directory / truncate_file).write_bytes(b"truncated")
    trace = directory / "must-not-run.csv"
    result = run(harness, directory, "--frames", "1", "--trace", str(trace),
                 "--trace-blocks", "10")
    if result.returncode == 0 or "Taito FX1S cycles=" in result.stderr:
        raise RuntimeError("invalid Taito FX-1S profile/media reached guest execution")
    if trace.is_file() and len(trace.read_text(encoding="utf-8").splitlines()) > 1:
        raise RuntimeError("invalid Taito FX-1S package reached the MIPS CPU")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, default=ROOT / "build/mac-harness/pcsx-harness")
    args = parser.parse_args()
    harness = args.harness.resolve()
    if not harness.is_file():
        raise SystemExit(f"harness not found: {harness}; build the native macOS BOARD=1 harness first")
    with tempfile.TemporaryDirectory(prefix="pcsx-taito-fx1-test-") as temporary:
        root = Path(temporary)
        valid = root / "sfchamp"
        make_media(valid)
        inputs = root / "inputs.csv"
        inputs.write_text("frame,control,pressed\n0,p1_button1,1\n1,p1_button1,0\n", encoding="ascii")
        trace, audio, nvram = root / "guest.csv", root / "sound.wav", root / "eeprom.bin"
        initial = bytes([0xff]) * 2048 + bytes(range(32))
        nvram.write_bytes(initial)
        result = run(harness, valid, "--board-test", "--frames", "3", "--inputs", str(inputs),
                     "--trace", str(trace), "--trace-blocks", "200000", "--audio", str(audio),
                     "--nvram", str(nvram), "--output", str(root / "frame.ppm"))
        check_guest(result, trace, audio)
        updated = nvram.read_bytes()
        if len(updated) != 2080 or updated[1] != 0x5a or updated[:1] != initial[:1] or updated[2:] != initial[2:]:
            raise RuntimeError("AT28C16 persisted data/ID differs from source program operation")

        cached = root / "sfchamp-cached"
        make_media(cached, cached=True)
        cached_trace, cached_audio = root / "cached.csv", root / "cached.wav"
        result = run(harness, cached, "--icache", "--frames", "3", "--inputs", str(inputs),
                     "--trace", str(cached_trace), "--trace-blocks", "200000",
                     "--audio", str(cached_audio), "--output", str(root / "cached.ppm"))
        check_guest(result, cached_trace, cached_audio)

        # No falling CK edge is emitted by this guest. Let the genuine five-
        # second watchdog expire, then verify the deferred reset runs at a
        # CPU boundary and the host still reaches the requested frame count.
        result = run(harness, valid, "--frames", "310", "--inputs", str(inputs),
                     "--output", str(root / "watchdog.ppm"))
        result.check_returncode()
        for marker in ("frames=310", "watchdog_requests=1 reset_pending=0 fault=0"):
            if marker not in result.stderr:
                raise RuntimeError(f"native watchdog reset did not complete: missing {marker!r}")

        for suffix, mutation, truncate in (("wrong-set", "set", None),
                                           ("wrong-geometry", "geometry", None),
                                           ("duplicate-key", "duplicate", None),
                                           ("truncated-audio", "none", "audiocpu.bin")):
            reject_before_guest(harness, valid, root / suffix, mutation, truncate)
    print("Taito FX-1S synthetic MIPS bank/GPU/EEPROM/SIO/IRQ, TC0140SYT and Z80/YM2610B PCM PASS")


if __name__ == "__main__":
    main()
