#!/usr/bin/env python3
"""Execute synthetic H8 audio firmware through the real System12 adapter.

Checks stream catch-up before key-on/off and reads, including the two cycles
around a sample edge. A long MIPS block changes board catch-up granularity;
audio must retain the same timed register behavior. No game assets are used.
"""
import argparse
import csv
from pathlib import Path
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]


def sub_program(edge_nops, read_flags=False):
    words = []

    def immediate(value):
        words.extend((0x7900, value))  # MOV.W #value,R0: 4 clocks

    def store(address, reg=0):
        words.extend((0x6ba0 | reg, address >> 16, address & 0xffff))

    def write(offset, value):
        immediate(value)
        store(0x280000 + offset * 2)  # 8 clocks after immediate

    def read_flags_to_shared(offset):
        words.extend((0x6b21, 0x0028, 0x0006))  # MOV.W @flags,R1
        store(0x080000 + offset, 1)

    write(0, 0xffff)
    write(2, 0xffff)
    write(6, 1 if read_flags else 63)
    write(3, 0x4004 if read_flags else 0x4006)
    immediate(0)
    words.extend([0x0000] * edge_nops)  # NOP: 2 clocks
    store(0x280404)
    # Reset prefetch is 6 clocks; four register writes cost 48, followed by
    # immediate/NOPs/store. The key-on bus timestamp is 66 + 2*edge_nops.
    keyon_cycle = 66 + 2 * edge_nops
    if read_flags:
        read_flags_to_shared(0)
        words.extend([0x0000] * 192)
        read_flags_to_shared(2)
    else:
        words.extend([0x0000] * 192)
        write(3, 0xa006)  # BUSY + KEYOFF + FILTER + LOOP
        write(0x202, 0)
    words.append(0x40fe)  # BRA self; no later audio commands
    sub = bytearray(0x80000)
    struct.pack_into('>I', sub, 0, 0x100)
    for i, word in enumerate(words):
        struct.pack_into('>H', sub, 0x100 + i * 2, word)
    return sub, keyon_cycle


def prepare(directory, edge_nops, coarse, read_flags):
    main = bytearray(0x400000)
    code = [0x3c081f08, 0x3c09800d]  # shared RAM and observable result RAM
    loop_pc = 0xbfc00000 + 4 * len(code)
    if coarse:
        code.extend([0] * 1024)
    # Consume the loads with stores so the DRC cannot discard dead results.
    # Snapshot RAM is observable even when the compiler retains host registers.
    code.extend((0x95100000, 0, 0xad302190, 0x95110002, 0, 0xad312194,
                 0x08000000 | ((loop_pc >> 2) & 0x03ffffff), 0))
    for i, word in enumerate(code):
        struct.pack_into('<I', main, i * 4, word)
    sub, keyon_cycle = sub_program(edge_nops, read_flags)
    (directory / 'main.bin').write_bytes(main)
    (directory / 'banked.bin').write_bytes(bytes(0x2000000))
    (directory / 'sub.bin').write_bytes(sub)
    (directory / 'wave.bin').write_bytes(bytes([0x40]) * 0x1000000)
    return keyon_cycle


def run_case(harness, directory, edge_nops, coarse, read_flags, drc):
    keyon = prepare(directory, edge_nops, coarse, read_flags)
    audio = directory / 'audio.wav'
    snapshot = directory / 'state.csv'
    command = [str(harness), '--system12', str(directory), '--frames', '1',
               '--audio', str(audio), '--output', str(directory / 'frame.ppm'),
               '--cycle-snapshots', str(snapshot)]
    if drc:
        command.append('--drc')
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    with wave.open(str(audio)) as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 88200)
        data = wav.readframes(wav.getnframes())
    frames = list(struct.iter_unpack('<hh', data))
    first = (keyon + 191) // 192
    end = first + 1 if read_flags else (keyon + 408 + 191) // 192
    expected = {i: (8 * (i - first + 1),) * 2 for i in range(first, end)}
    observed = {i: sample for i, sample in enumerate(frames) if sample != (0, 0)}
    assert observed == expected, (edge_nops, coarse, read_flags, drc, expected, observed)
    if read_flags:
        with snapshot.open() as stream:
            states = list(csv.DictReader(stream))
        assert states, 'no CPU snapshots'
        # The second flags read must catch up the one-shot before inspecting
        # BUSY, even with no intervening board sync inside a long MIPS block.
        assert int(states[-1]['ram_d2190'], 16) == 0x8004, states[-1]
        assert int(states[-1]['ram_d2194'], 16) == 0x2004, states[-1]
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--drc', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-c352-timing-') as tmp:
        root = Path(tmp)
        for edge in (62, 63, 64):  # key-on at H8 clocks 190, 192 and 194
            outputs = []
            for coarse in (False, True):
                directory = root / f'{edge}-{int(coarse)}'
                directory.mkdir()
                outputs.append(run_case(args.harness.resolve(), directory, edge,
                                        coarse, False, args.drc))
            # Run endpoints differ slightly with block sizes; compare their
            # complete common interval, including the trailing silence.
            count = min(map(len, outputs))
            assert outputs[0][:count] == outputs[1][:count]
        for coarse in (False, True):
            directory = root / f'flags-{int(coarse)}'
            directory.mkdir()
            run_case(args.harness.resolve(), directory, 0, coarse, True, args.drc)
    print('System12 H8/C352 timed key-on/off, register-read and slice tests PASS')


if __name__ == '__main__':
    main()
