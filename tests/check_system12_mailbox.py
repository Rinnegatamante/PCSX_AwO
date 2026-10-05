#!/usr/bin/env python3
"""Compare a real PSX/H8 mailbox exchange against MAME, with zero tolerance.

This deliberately exposes the current scheduler mismatch. Successful fixture
and passive-observer checks do not make the timing comparison pass. Neither
CPU timestamps nor the scheduling policy are adjusted to match the oracle.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from check_system12_vblank import prepare

MAIN_PERIOD_AS = 20_000_000_000  # Native 50 MHz counter, two counts/instruction.
H8_PERIOD_AS = 10**18 // 16_934_400  # MAME attotime::from_hz, cold clock epoch.


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def observations(rows):
    """Seven semantic observations, retaining each CPU's actual bus time.

    MAME's compression can emit the same pending read twice. Deduplicate its
    observation, not separate memory transactions. Do not sort by timestamp:
    CPU local times overlap within a scheduler slice.
    """
    unique = []
    for row in rows:
        if row not in unique:
            unique.append(row)
    assert len(unique) == 7 and all(r['width'] == 16 for r in unique), unique
    writes = [r for r in unique if r['kind'] == 'write']
    assert [(r['cpu'], r['address'], r['value'], r['width']) for r in writes] == [
        ('main', 0x1f080000, 0x1122, 16),
        ('sub', 0x080002, 0x3344, 16),
        ('main', 0x1f080004, 0x3344, 16)], writes
    old = [r for r in unique if r['cpu'] == 'main' and r['kind'] == 'read'
           and r['address'] == 0x1f080002 and r['value'] == 0]
    new = [r for r in unique if r['cpu'] == 'main' and r['kind'] == 'read'
           and r['address'] == 0x1f080002 and r['value'] == 0x3344]
    command = [r for r in unique if r['cpu'] == 'sub' and r['kind'] == 'read'
               and r['address'] == 0x080000 and r['value'] == 0x1122]
    assert len(old) == 2 and len(new) == 1 and len(command) == 1, unique
    return dict(main_publish=writes[0], main_first_zero=old[0],
                main_last_zero=old[-1], h8_command=command[0],
                h8_response=writes[1], main_response=new[0], main_ack=writes[2])


def run_case(harness, directory, words, drc, vblank_only):
    directory.mkdir()
    prepare(directory, False)
    for name, offset, fmt in [('main', 0, 'I'), ('sub', 0x100, 'H')]:
        with (directory / (name + '.bin')).open('r+b') as stream:
            stream.seek(offset)
            stream.write(struct.pack(('<' if name == 'main' else '>') +
                                     fmt * len(words[name]), *words[name]))
    common = [str(harness), '--system12', str(directory), '--frames', '2']
    if drc:
        common.append('--drc')
    if vblank_only:
        common.append('--system12-vblank-sync-only')
    command = common + ['--system12-edge-trace', str(directory / 'edges.csv'),
                        '--system12-trace-shared-accesses', '--output',
                        str(directory / 'frame.ppm')]
    control_command = common + ['--output', str(directory / 'control.ppm')]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    control = subprocess.run(control_command, capture_output=True, text=True, timeout=30)
    for run in (result, control):
        assert run.returncode == 0, run.stderr
        assert 'overflow' not in run.stderr and 'halted=1' not in run.stderr, run.stderr
        h8 = re.search(r'illegal=([0-9a-f]+):([0-9a-f]+) unsupported=([0-9a-f]+)', run.stderr)
        assert h8 and not any(int(v, 16) for v in h8.groups()), run.stderr
        assert 'unmapped reads=0 writes=0' in run.stderr, run.stderr
    report = lambda run: [s for s in run.stderr.splitlines() if s.startswith('System12:')]
    assert report(result) == report(control), (report(result), report(control))
    assert digest(directory / 'frame.ppm') == digest(directory / 'control.ppm')
    with (directory / 'edges.csv').open() as stream:
        rows = [{k: v if k == 'event' else int(v) for k, v in row.items()}
                for row in csv.DictReader(stream)]
    assert all(a['sequence'] < b['sequence'] for a, b in zip(rows, rows[1:])), rows
    kinds = dict(main_shared_load=('main', 'read'), main_shared_store=('main', 'write'),
                 h8_load=('sub', 'read'), h8_store=('sub', 'write'))
    normalized = []
    for row in rows:
        if row['event'] not in kinds:
            continue
        cpu, kind = kinds[row['event']]
        normalized.append(dict(cpu=cpu, kind=kind, address=row['address'],
                               value=row['value'], width=row['width'],
                               time_as=row['main_cycle'] * MAIN_PERIOD_AS if cpu == 'main'
                               else row['h8_cycle'] * H8_PERIOD_AS))
    assert 'shared r=2 w=2' in result.stderr, result.stderr
    return dict(cpu='drc' if drc else 'interpreter', vblank_sync_only=vblank_only,
                command=command, control_command=control_command,
                observer_control_equal=True, fixture_passed=True, rows=rows,
                observations=observations(normalized), stderr=result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--mame', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    harness = args.harness.resolve()
    oracle = json.loads(args.mame.read_text())
    assert oracle['schema'] == 'pcsx-aow.mame-system12-mailbox.v1'
    assert oracle['clock']['h8_hz'] == 16934400
    assert oracle['clock']['mame_main_scheduler_hz'] == 25000000
    assert len(oracle['runs']) == 1 and oracle['runs'][0]['returncode'] == 0
    assert not oracle['runs'][0]['timed_out'] and oracle['ack_complete']
    assert len(oracle['reset_events']) == 1
    assert oracle['reset_events'][0]['time'] == dict(seconds=0, attoseconds=0)
    hashes = oracle['rom_hashes']
    for field in ('parent_rom', 'lua_template'):
        assert hashes[field + '_before'] == hashes[field + '_after']
    words = {cpu: [int(w, 0) for w in values]
             for cpu, values in oracle['firmware_words'].items()}
    assert words['main'][11] == 0x0bf0000b, 'firmware must park after ACK'
    expected_rows = []
    for row in oracle['events']:
        mask = row['mem_mask']
        width = mask.bit_count()
        assert width == 16, row
        expected_rows.append(dict(cpu=row['cpu'], kind='write' if row['kind'] == 'write'
                                  else 'read', address=row['address'], value=row['value'],
                                  width=width, time_as=row['time']['seconds'] * 10**18 +
                                  row['time']['attoseconds']))
    expected = observations(expected_rows)
    captures = []
    with tempfile.TemporaryDirectory(prefix='pcsx-mailbox-probe-') as tmp:
        for drc in (False, True):
            for sync in (False, True):
                case = run_case(harness, Path(tmp) / f'{int(drc)}-{int(sync)}', words, drc, sync)
                case['differences'] = [dict(observation=key, mame=want,
                                           native=case['observations'][key],
                                           time_delta_as=case['observations'][key]['time_as'] -
                                           want['time_as'])
                                       for key, want in expected.items()
                                       if want != case['observations'][key]]
                case['equal'] = not case['differences']
                captures.append(case)
                print(f"{case['cpu']} vblank_only={int(sync)}: fixture PASS, "
                      f"{len(case['differences'])}/7 MAME differences", flush=True)
    evidence = dict(schema='pcsx-aow.native-system12-mailbox.v1', harness=str(harness),
                    harness_sha256=digest(harness), mame_evidence=str(args.mame.resolve()),
                    mame_evidence_sha256=digest(args.mame), firmware_words=words,
                    main_counter_hz=50000000, h8_period_as=H8_PERIOD_AS,
                    comparison='exact bus address/value/width and reset-relative attoseconds',
                    scope=__doc__, expected=expected, captures=captures,
                    fixture_passed=True, equal=all(c['equal'] for c in captures))
    args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print('System12 mailbox MAME fidelity ' + ('PASS' if evidence['equal'] else 'FAIL'))
    return int(not evidence['equal'])


if __name__ == '__main__':
    raise SystemExit(main())
