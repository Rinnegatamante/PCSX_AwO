#!/usr/bin/env python3
"""Execute a warm System12 reset through the native core, with synthetic ROMs.

Observe guest RAM/shared words/banked fetches and held controls after restart,
plus the absolute C352 sample phase of the restarted H8 firmware.
"""
import argparse
import csv
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import wave

from check_system12_audio_timing import prepare


def main_program(directory, variant):
    # Count boots in RAM. Only the first boot initializes markers and bank 2;
    # the next boot reads them, so reset cannot recreate the expected state.
    code = [0x3c08800d, 0x8d092198, 0, 0x252a0001, 0xad0a2198,
            0x15200000, 0, 0x3c0a1234, 0x354a5678, 0xad0a21a0,
            0x3c0b1f08, 0x340a1357, 0xa56a0010,
            0x3c0c1f00, 0x340a0002, 0xa58a0000,
            0x3c0d8001, 0x3c0a89ab, 0x354acdef, 0xadaa0000, 0xadaa000c]
    if variant:
        code.extend((0x3c0e80f0, 0xadca0000))
    loop = len(code)
    code[5] |= loop - 6
    loop_pc = 0xbfc00000 + loop * 4
    code.extend((0x3c0b1fa0, 0x8d6a0000, 0, 0xad0a2190,
                 0x3c0b1f08, 0x956a0010, 0, 0xad0a2194,
                 0x8d0a21a0, 0, 0xad0a219c,
                 0x3c0d8001, 0x8daa0000, 0, 0xad0a21a4,
                 0x8daa000c, 0, 0xad0a21a8))
    if variant:
        code.extend((0x3c0e80f0, 0x8dca0000, 0, 0xad0a21ac))
    code.extend((0x08000000 | ((loop_pc >> 2) & 0x03ffffff), 0))
    rom = bytearray(0x400000)
    for i, word in enumerate(code):
        struct.pack_into('<I', rom, i * 4, word)
    if variant:
        rom[0x20280:0x2028c] = b'BOOT-HACK-12'
        (directory / 'board.cfg').write_text(
            'schema=1\nmame_set=sws2001\nmachine=coh716\nstate=namcos12_altbank_state\n'
            'ram_size=16777216\nmain_size=4194304\nbank_size=33554432\n'
            'sub_size=524288\nwave_size=16777216\nboot_hack=1\nalt_bank=1\n'
            'tektagt_dma=0\nunsupported=none\n')
    (directory / 'main.bin').write_bytes(rom)
    banked = bytearray(0x2000000)
    for bank in range(16):
        struct.pack_into('<I', banked, bank * 0x200000, 0xb00b0000 | bank)
    (directory / 'banked.bin').write_bytes(banked)


def check(harness, directory, at, drc, variant=False):
    keyon = prepare(directory, 62, False, False)  # 190 H8 clocks
    main_program(directory, variant)
    inputs = directory / 'inputs.csv'
    inputs.write_text('frame,control,pressed\n0,p1start,1\n0,coin1,1\n')
    audio, snapshot = directory / 'audio.wav', directory / 'state.csv'
    command = [str(harness), '--system12', str(directory), '--frames', '2' if at > 500000 else '1',
               '--reset-at-cycle', str(at), '--inputs', str(inputs),
               '--audio', str(audio), '--cycle-snapshots', str(snapshot),
               '--output', str(directory / 'frame.ppm')]
    if drc:
        command.append('--drc')
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    match = re.search(r'System12 harness soft reset: cycle=(\d+)', result.stderr)
    assert match, 'the requested reset was not executed'
    reset_cycle = int(match.group(1))
    clock = re.search(r'clock=(\d+) cycle_multiplier=', result.stderr)
    assert clock, result.stderr
    hz = int(clock.group(1))
    h8_base = reset_cycle * 16934400 // hz
    # Positive H8 time renders floor(time*rate)+1 samples before register access.
    # The pre-reset catch-up includes the retained sub-256 PSX-cycle remainder.
    period = 10**18 // 16934400
    target = lambda cycle: cycle * period * 88200 // 10**18 + 1
    if at == 835756:
        # MAME's frame-done reset oracle runs at 16.715113233669674 ms.
        # This main-cycle request lands in the same C352 phase bin; its second
        # burst is at sample 1476 and becomes silent at 1478 in actual MAME.
        assert target(h8_base + keyon) == 1476
        assert target(h8_base + keyon + 408) == 1478
    expected = {}
    for base in (0, h8_base):
        first, end = target(base + keyon), target(base + keyon + 408)
        assert first >= 0 and end > first
        for i in range(first, end):
            expected[i] = (8 * (i - first + 1),) * 2
    with wave.open(str(audio)) as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 88200)
        frames = list(struct.iter_unpack('<hh', wav.readframes(wav.getnframes())))
    observed = {i: sample for i, sample in enumerate(frames) if sample != (0, 0)}
    assert observed == expected, (at, drc, reset_cycle, h8_base, expected, observed)
    with snapshot.open() as stream:
        states = list(csv.DictReader(stream))
    assert states, 'no snapshots'
    final = states[-1]
    retained = {'ram_d2190': 0xb00b0002, 'ram_d2194': 0x1357,
                'ram_d2198': 2, 'ram_d219c': 0x12345678, 'ram_d21a0': 0x12345678,
                'ram_d21a4': 0x544f4f42 if variant else 0x89abcdef,
                'ram_d21a8': 0x89abcdef}
    if variant:
        retained['ram_d21ac'] = 0x89abcdef
    for field, value in retained.items():
        assert int(final[field], 16) == value, (field, value, final)
    assert 'System12: JAMMA=ff/7f/df/ff' in result.stderr, result.stderr
    assert 'unsupported=0' in result.stderr and 'halted=0' in result.stderr, result.stderr
    print(f'{"COH716/altbank" if variant else "COH700"}: reset requested={at} actual={reset_cycle} H8 base={h8_base} '
          f'second onset={target(h8_base + keyon)}: RAM/shared/bank/input/PCM PASS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--drc', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pcsx-system12-reset-') as tmp:
        for at in (2000, 4000, 10000, 835756):
            directory = Path(tmp) / str(at)
            directory.mkdir()
            check(args.harness.resolve(), directory, at, args.drc)
            shutil.rmtree(directory)
        directory = Path(tmp) / 'coh716'
        directory.mkdir()
        check(args.harness.resolve(), directory, 2000, args.drc, True)
    print('System12 executed soft-reset retention and absolute C352 phase tests PASS')


if __name__ == '__main__':
    main()
