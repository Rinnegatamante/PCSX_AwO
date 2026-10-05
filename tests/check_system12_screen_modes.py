#!/usr/bin/env python3
"""Check native System 12 GP1 mode changes against absolute H8 vblank cycles."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN_SIZE = 0x400000
BANK_SIZE = 0x2000000
SUB_SIZE = 0x80000
WAVE_SIZE = 0x1000000
MAIN_HZ = 50_000_000
H8_HZ = 16_934_400
ATTO = 10**18
H8_WORDS = (0x6A08, 0xFFD6, 0x40FA)  # MOV.B @ffffd6,R0L; BRA -6
NTSC240 = 16_715_113_233_669_674
NTSC480 = 16_683_335_451_858_734
PAL = 20_000_000_000_000_000
BLANK = 2_500_000_000_000_000


def sparse_region(path: Path, size: int, writes: tuple[tuple[int, bytes], ...] = ()) -> None:
    with path.open("wb") as stream:
        stream.truncate(size)
        for offset, data in writes:
            stream.seek(offset)
            stream.write(data)


def main_program(mode_words: tuple[int, ...], delay_nops: int) -> tuple[bytes, int, int, bytes]:
    """Make a reset-safe MIPS program and return bytes plus post-write spin PC."""
    words: list[int] = []
    words.extend((0x3C08800D, 0x350821E0, 0x8D090000, 0x00000000))
    beq_index = len(words)
    words.extend((0x11200000, 0x00000000))  # beq t1,zero,cold; delay slot
    spin_jump_index = len(words)
    words.extend((0x08000000, 0x00000000))  # warm reset jumps to the idle loop
    cold_index = len(words)
    words.extend((0x24090001, 0xAD090000))  # persistent one-shot boot guard
    words.extend((0x3C0A1F80, 0x354A1814))  # GP1 address
    # Branch displacement counts from the instruction after the branch.
    words[beq_index] |= (cold_index - beq_index - 1) & 0xFFFF
    loop_pc = 0xBFC00000 + spin_jump_index * 4
    words[spin_jump_index] |= (loop_pc >> 2) & 0x03FFFFFF
    head = struct.pack("<" + "I" * len(words), *words)
    suffix_index = len(words) + delay_nops
    suffix = []
    for mode_word in mode_words:
        suffix.extend((0x3C0B0000 | ((mode_word >> 16) & 0xFFFF),
                       0x356B0000 | (mode_word & 0xFFFF), 0xAD4B0000))
    suffix.extend((0x08000000 | ((loop_pc >> 2) & 0x03FFFFFF), 0))
    suffix_bytes = struct.pack("<" + "I" * len(suffix), *suffix)
    suffix_offset = suffix_index * 4
    assert suffix_offset + len(suffix_bytes) <= MAIN_SIZE
    return head, loop_pc, suffix_offset, suffix_bytes


def prepare(directory: Path, mode_words: tuple[int, ...], delay_nops: int) -> int:
    head, spin_pc, suffix_offset, suffix = main_program(mode_words, delay_nops)
    sparse_region(directory / "main.bin", MAIN_SIZE,
                  ((0, head), (suffix_offset, suffix)))
    sparse_region(directory / "banked.bin", BANK_SIZE)
    sub = bytearray(0x106)
    struct.pack_into(">I", sub, 0, 0x100)
    struct.pack_into(">HHH", sub, 0x100, *H8_WORDS)
    sparse_region(directory / "sub.bin", SUB_SIZE, ((0, bytes(sub)),))
    sparse_region(directory / "wave.bin", WAVE_SIZE)
    (directory / "board.cfg").write_text(
        "schema=1\nmame_set=soulclbr\nmachine=coh700\nstate=namcos12_state\n"
        "ram_size=4194304\nmain_size=4194304\nbank_size=33554432\n"
        "sub_size=524288\nwave_size=16777216\nboot_hack=0\nalt_bank=0\n"
        "tektagt_dma=0\nunsupported=none\n")
    return spin_pc


def absolute_h8_cycle(attoseconds: int) -> int:
    return attoseconds // (ATTO // H8_HZ)


def schedule(period: int, starts: int = 3) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    start = period
    for _ in range(starts):
        result.extend(((1, start), (0, start + BLANK)))
        start += period
    return result


def read_edges(path: Path) -> list[dict[str, int | str]]:
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    return [{key: value if key == "event" else int(value)
             for key, value in row.items()} for row in rows]


def trace_cycle_after_write(path: Path, spin_pc: int) -> int:
    with path.open(newline="") as stream:
        rows = csv.DictReader(stream)
        for row in rows:
            if int(row["pc"], 16) == spin_pc:
                return int(row["cycle"])
    raise AssertionError(f"MIPS execution trace never reached post-write loop PC 0x{spin_pc:08x}")


def run_case(harness: Path, directory: Path, name: str, mode_words: tuple[int, ...],
             delay_nops: int, drc: bool, reset_at: int | None = None,
             expected: list[tuple[int, int]] | None = None,
             during_window: tuple[int, int] | None = None) -> dict:
    spin_pc = prepare(directory, mode_words, delay_nops)
    edge_path = directory / "edges.csv"
    trace_path = directory / "mips.csv"
    command = [str(harness), "--system12", str(directory), "--frames", "3",
               "--system12-edge-trace", str(edge_path), "--trace", str(trace_path),
               "--trace-blocks", "10000", "--output", str(directory / "frame.ppm")]
    if reset_at is not None:
        command.extend(("--reset-at-cycle", str(reset_at)))
    if drc:
        command.append("--drc")
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(f"{name} ({'DRC' if drc else 'interpreter'}) failed:\n{result.stderr}")
    rows = read_edges(edge_path)
    edges = [row for row in rows if row["event"] == "vblank"]
    reads = [row for row in rows if row["event"] == "portb_input"]
    transitions = [(old, new) for old, new in zip(reads, reads[1:])
                   if (int(old["value"]) ^ int(new["value"])) & 0x80]
    if expected is None:
        raise AssertionError("expected edge schedule must be supplied")
    observed = [(int(row["value"]), int(row["h8_cycle"])) for row in edges[:len(expected)]]
    expected_cycles = [(level, absolute_h8_cycle(at)) for level, at in expected]
    assert observed == expected_cycles, (name, drc, observed, expected_cycles, result.stderr)
    assert len(edges) >= len(expected), (name, len(edges), expected, result.stderr)
    assert transitions, (name, "H8 port-B poll transitions missing", rows[:20])
    assert len(transitions) >= 2, (name, "H8 polling loop did not observe both vblank levels", transitions)
    actual_reset = None
    reset_match = re.search(r"System12 harness soft reset: cycle=(\d+)", result.stderr)
    if reset_at is not None:
        assert reset_match, (name, "warm reset did not run", result.stderr)
        actual_reset = int(reset_match.group(1))
        assert actual_reset >= reset_at, (actual_reset, reset_at)
    if name == "warm_reset":
        assert actual_reset is not None
        reset_time = actual_reset * ATTO // MAIN_HZ
        retained_start = NTSC480
        age_at_reset = reset_time - retained_start
        remaining_phase = NTSC240 - age_at_reset
        phase_start = reset_time + remaining_phase
        assert 0 <= age_at_reset < NTSC240, (actual_reset, reset_time, age_at_reset)
        assert phase_start == retained_start + NTSC240, (phase_start, retained_start, actual_reset)
        assert int(edges[2]["h8_cycle"]) == absolute_h8_cycle(phase_start), (name, edges[2], phase_start)
    if during_window:
        start, end = during_window
        after_write_cycle = trace_cycle_after_write(trace_path, spin_pc)
        after_write_time = after_write_cycle * ATTO // MAIN_HZ
        assert start < after_write_time < end, (name, after_write_cycle,
                                                after_write_time, start, end)
    print(f"{name} {'DRC' if drc else 'interpreter'}: "
          + ", ".join(f"{'START' if level else 'END'}@H8={cycle}"
                       for level, cycle in observed[:len(expected)])
          + (f" reset={actual_reset}" if actual_reset is not None else ""))
    return {"command": command, "edges": edges, "actual_reset_cycle": actual_reset,
            "stderr": result.stderr}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--drc", action="store_true")
    args = parser.parse_args()
    harness = args.harness.resolve()
    if not harness.is_file():
        parser.error(f"harness not found: {harness}")

    cases = [
        ("ntsc480", (0x08000004,), 32, None, schedule(NTSC480)),
        ("pal50", (0x08000008,), 32, None, schedule(PAL)),
        ("gpu_default_reset", (0x08000008, 0x00000000), 32, None, schedule(NTSC240)),
        ("change_during_blank", (0x08000008,), 425_000, None,
         [(1, NTSC240), (0, NTSC240 + BLANK),
          (1, NTSC240 + PAL), (0, NTSC240 + PAL + BLANK),
          (1, NTSC240 + 2 * PAL), (0, NTSC240 + 2 * PAL + BLANK)]),
    ]
    warm_expected = [(1, NTSC480), (0, NTSC480 + BLANK),
                     (1, NTSC480 + NTSC240), (0, NTSC480 + NTSC240 + BLANK),
                     (1, NTSC480 + 2 * NTSC240), (0, NTSC480 + 2 * NTSC240 + BLANK)]
    with tempfile.TemporaryDirectory(prefix="pcsx-system12-screen-modes-") as tmp:
        root = Path(tmp)
        for name, modes, delay, reset_at, expected in cases:
            directory = root / name
            directory.mkdir()
            window = ((NTSC240, NTSC240 + BLANK) if name == "change_during_blank" else None)
            run_case(harness, directory, name, modes, delay, args.drc,
                     reset_at, expected, window)
        directory = root / "warm_reset"
        directory.mkdir()
        run_case(harness, directory, "warm_reset", (0x08000004,), 32, args.drc,
                 850_000, warm_expected)
    print("System12 native GP1 screen modes, blank retention and warm-reset phase PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
