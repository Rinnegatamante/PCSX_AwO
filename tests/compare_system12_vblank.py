#!/usr/bin/env python3
"""Compare recorded H8 port-B transition cycles against the MAME capture.

Returns failure on any cycle difference. This is a narrow polling-fixture gate,
not complete board equivalence. It deliberately accepts no timing tolerance.
"""
import argparse
import json
from pathlib import Path

H8_HZ = 16934400
AS_PER_CYCLE = 10**18 // H8_HZ


def attoseconds(parts):
    return parts['seconds'] * 10**18 + parts['attoseconds']


def mame_cycles(parts):
    value = attoseconds(parts)
    cycle, remainder = divmod(value, AS_PER_CYCLE)
    if remainder:
        raise ValueError(f'MAME read time {value} is not on the initial H8 cycle grid')
    return cycle


def native_transitions(capture):
    reads = [row for row in capture['rows'] if row['event'] == 'portb_input']
    return [(old, new) for old, new in zip(reads, reads[1:])
            if (old['value'] ^ new['value']) & 0x80]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame', type=Path, required=True)
    parser.add_argument('--native', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    oracle = json.loads(args.mame.read_text())
    if oracle['mame_returncode'] != 0 or oracle.get('timed_out'):
        raise ValueError('MAME capture did not exit successfully')
    if oracle['firmware_words'] != ['0x6a08', '0xffd6', '0x40fa']:
        raise ValueError('MAME capture uses different H8 polling firmware')
    # Refuse to silently align two different reset epochs or instruction phases.
    if mame_cycles(oracle['first_read']['time']) != 12:
        raise ValueError('MAME first poll does not start at initial H8 cycle 12')
    resets = oracle.get('reset_events', [])
    if len(resets) != 1 or attoseconds(resets[0]['time']) != 0:
        raise ValueError('comparison requires one reset epoch at initial time zero')
    expected = oracle['transitions'][:4]
    if len(expected) != 4:
        raise ValueError('MAME capture has fewer than four transitions')
    results = []
    for path in args.native:
        native = json.loads(path.read_text())
        if native['h8_hz'] != H8_HZ or native['main_cycle_hz'] != 50000000:
            raise ValueError('native capture uses different clocks')
        if native['h8_firmware_words'] != [0x6a08, 0xffd6, 0x40fa]:
            raise ValueError('native capture uses different H8 polling firmware')
        for capture in native['captures']:
            actual = native_transitions(capture)
            if len(actual) < 4:
                raise ValueError('native capture has fewer than four transitions')
            rows = []
            for index, (reference, (old, new)) in enumerate(zip(expected, actual)):
                before = mame_cycles(reference['before_time'])
                after = mame_cycles(reference['after_time'])
                same = (old['h8_cycle'], new['h8_cycle'], old['value'], new['value']) == (
                    before, after, reference['before'], reference['after'])
                rows.append(dict(transition=index + 1,
                                 mame_before=before, mame_after=after,
                                 native_before=old['h8_cycle'], native_after=new['h8_cycle'],
                                 first_read_delta=new['h8_cycle'] - after, equal=same))
                print(f"{capture['cpu']} coarse={int(capture['coarse'])} "
                      f"edge={index + 1}: MAME first={after}, "
                      f"native first={new['h8_cycle']}, "
                      f"delta={new['h8_cycle'] - after} H8 cycles")
            results.append(dict(cpu=capture['cpu'], coarse=capture['coarse'],
                                native_evidence=str(path), rows=rows))
    equal = all(row['equal'] for result in results for row in result['rows'])
    if args.output:
        args.output.write_text(json.dumps(dict(
            mame_evidence=str(args.mame), equal=equal, results=results,
            scope='four port-B polling transitions; zero tolerance; no complete board claim'),
            indent=2) + '\n')
    print('System12 vblank polling fidelity ' + ('PASS' if equal else 'FAIL'))
    return 0 if equal else 1


if __name__ == '__main__':
    raise SystemExit(main())
