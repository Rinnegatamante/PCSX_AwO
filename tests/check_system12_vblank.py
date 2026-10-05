#!/usr/bin/env python3
"""Capture System12 vblank delivery and H8 port-B visibility without rescheduling.

The H8 polls port B with the same six-byte firmware as the MAME oracle.
Fine/coarse MIPS blocks deliberately expose the current adapter's sync policy.
This validates the diagnostic fixture; it does not assert MAME equivalence.
"""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import tempfile

H8_WORDS = (0x6a08, 0xffd6, 0x40fa)  # MOV.B @ffffd6,R0L; BRA 0x100


def sparse_region(path, size, writes=()):
    with path.open('wb') as stream:
        stream.truncate(size)
        for offset, data in writes:
            stream.seek(offset)
            stream.write(data)


def prepare(directory, coarse):
    code = [0] * (1024 if coarse else 0) + [0x0bf00000, 0]
    sparse_region(directory / 'main.bin', 0x400000,
                  [(0, struct.pack('<' + 'I' * len(code), *code))])
    sparse_region(directory / 'banked.bin', 0x2000000)
    sparse_region(directory / 'wave.bin', 0x1000000)
    sparse_region(directory / 'sub.bin', 0x80000,
                  [(0, struct.pack('>I', 0x100)),
                   (0x100, struct.pack('>HHH', *H8_WORDS))])


def run_case(harness, directory, coarse, drc, vblank_sync_only=False):
    prepare(directory, coarse)
    trace = directory / 'edges.csv'
    command = [str(harness), '--system12', str(directory), '--frames', '3',
               '--system12-edge-trace', str(trace),
               '--output', str(directory / 'frame.ppm')]
    if drc:
        command.append('--drc')
    if vblank_sync_only:
        command.append('--system12-vblank-sync-only')
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    control_command = [str(harness), '--system12', str(directory), '--frames', '3',
                       '--output', str(directory / 'control.ppm')]
    if drc:
        control_command.append('--drc')
    if vblank_sync_only:
        control_command.append('--system12-vblank-sync-only')
    control = subprocess.run(control_command, capture_output=True, text=True, timeout=30)
    if control.returncode:
        raise RuntimeError(control.stderr)
    # The observer must not change board execution, counters or output.
    observed_state = [line for line in result.stderr.splitlines()
                      if line.startswith('System12:')]
    control_state = [line for line in control.stderr.splitlines()
                     if line.startswith('System12:')]
    assert observed_state == control_state, (observed_state, control_state)
    assert (directory / 'frame.ppm').read_bytes() == (directory / 'control.ppm').read_bytes()
    pcm = re.search(r'System12: C352 rate=88200 frames=(\d+)', result.stderr)
    assert pcm and int(pcm.group(1)) > 0, ('PCM clock did not advance', result.stderr)
    with trace.open() as stream:
        rows = [{key: value if key == 'event' else int(value)
                 for key, value in row.items()} for row in csv.DictReader(stream)]
    assert rows and all(a['sequence'] < b['sequence']
                        for a, b in zip(rows, rows[1:])), rows
    reads = [row for row in rows if row['event'] == 'portb_input']
    edges = [row for row in rows if row['event'] == 'vblank']
    assert reads[0]['h8_cycle'] == 12, reads[0]
    assert all(row['h8_cycle'] % 10 == 2 for row in reads), reads
    assert len(edges) == 6, edges
    assert [row['value'] for row in edges] == [1, 0] * 3, edges
    transitions = [(old, new) for old, new in zip(reads, reads[1:])
                   if (old['value'] ^ new['value']) & 0x80]
    # The frame endpoint may stop before the last falling-edge poll.
    assert len(transitions) >= 5, transitions
    for edge in edges:
        edge['pending_main_cycles'] = (edge['main_cycle'] -
                                       edge['synced_main_cycle']) & 0xffffffff
    for index, (old, new) in enumerate(transitions):
        print(f"{'drc' if drc else 'interpreter'} coarse={int(coarse)} "
              f"transition={index + 1} old_H8={old['h8_cycle']} "
              f"new_H8={new['h8_cycle']} bit7={new['value'] >> 7}")
    print('  unsynchronized main-cycle tails:',
          [edge['pending_main_cycles'] for edge in edges])
    return dict(coarse=coarse, cpu='drc' if drc else 'interpreter',
                vblank_sync_only=vblank_sync_only,
                command=command, rows=rows, stderr=result.stderr,
                observer_control_equal=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--drc', action='store_true')
    parser.add_argument('--vblank-sync-only', action='store_true',
                        help='omit frontend block catch-up, keeping core screen events')
    parser.add_argument('--output', type=Path, help='save captured observations as JSON')
    args = parser.parse_args()
    harness = args.harness.resolve()
    captures = []
    with tempfile.TemporaryDirectory(prefix='pcsx-vblank-probe-') as tmp:
        directory = Path(tmp)
        for coarse in (False, True):
            captures.append(run_case(harness, directory, coarse, args.drc,
                                     args.vblank_sync_only))
    evidence = dict(harness=str(harness),
                    harness_sha256=hashlib.sha256(harness.read_bytes()).hexdigest(),
                    main_cycle_hz=50000000, h8_hz=16934400,
                    h8_firmware_words=list(H8_WORDS), captures=captures,
                    scope='native observations; no MAME equivalence assertion')
    if args.output:
        args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print('System12 native vblank diagnostic fixture PASS (fidelity comparison pending)')


if __name__ == '__main__':
    main()
