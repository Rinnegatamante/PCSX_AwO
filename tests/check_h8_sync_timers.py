#!/usr/bin/env python3
"""Strictly compare native SCI global timer fields with the real MAME capture.

This isolated H8 fixture owns/fires the empty timer callbacks in one-cycle
steps. It does not implement the PSX/H8 production scheduler or claim that the
shared-mailbox timing mismatch has been fixed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    oracle = json.loads(args.mame.read_text())
    assert oracle['schema'] == 'pcsx-aow.mame-h8-sync-timers.v1'
    assert oracle['h8_hz'] == 16934400
    assert oracle['parent_rom_before'] == oracle['parent_rom_after']
    assert oracle['template_before'] == oracle['template_after']
    assert sorted((c['channel'], c['stopped']) for c in oracle['captures']) == [
        (0, False), (0, True), (1, False), (1, True)]
    captures = []
    with tempfile.TemporaryDirectory(prefix='native-h8-sync-') as tmp:
        for expected in oracle['captures']:
            assert len(expected['rows']) == 96
            assert [r['index'] for r in expected['rows']] == list(range(1, 97))
            assert expected['returncode'] == 0 and not expected['timed_out']
            assert expected['resets'] == [dict(seconds=0, attoseconds=0)]
            rom = bytearray([255])*0x80000
            struct.pack_into('>I', rom, 0, 0x100)
            words = expected['firmware_words']
            struct.pack_into('>'+'H'*len(words), rom, 0x100, *words)
            path = Path(tmp)/'sub.bin'
            path.write_bytes(rom)
            command = [str(args.native.resolve()), str(path), str(expected['channel']),
                       str(len(expected['rows']))]
            run = subprocess.run(command, text=True, capture_output=True, timeout=30)
            assert run.returncode == 0, run.stdout + run.stderr
            rows = [dict(index=int(n), cycle=int(c), armed=bool(int(a)),
                         expire=dict(seconds=int(s), attoseconds=int(t)), clock_event=int(e))
                    for n,c,a,s,t,e in re.findall(
                        r'NATIVE_SYNC n=(\d+) cycle=(\d+) armed=(\d+) '
                        r'expire=(\d+):(\d+) clock_event=(\d+)', run.stdout)]
            assert len(rows) == len(expected['rows'])
            reference = [{key: row[key] for key in ('index','cycle','armed','expire','clock_event')}
                         for row in expected['rows']]
            actual = [dict(row) for row in rows]
            differences = [dict(index=i, mame=m, native=n) for i,(m,n) in
                           enumerate(zip(reference, actual)) if m != n]
            captures.append(dict(channel=expected['channel'], stopped=expected['stopped'],
                                 command=command, rows=rows, differences=differences,
                                 equal=not differences))
            print(f"SCI{expected['channel']} stopped={int(expected['stopped'])}: "
                  f"{len(rows)} comparisons, {len(differences)} differences", flush=True)
    equal = all(c['equal'] for c in captures)
    args.output.write_text(json.dumps(dict(schema='pcsx-aow.native-h8-sync-timers.v1',
        native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
        mame_sha256=hashlib.sha256(args.mame.read_bytes()).hexdigest(),
        captures=captures, equal=equal, scope=__doc__), indent=2)+'\n')
    print('H8 SCI global timer field comparison ' + ('PASS' if equal else 'FAIL'))
    return int(not equal)


if __name__ == '__main__':
    raise SystemExit(main())
