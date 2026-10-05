#!/usr/bin/env python3
"""Capture actual SCI one-shot timer fields separately from H8 clock deadlines.

Timers are identified by their source save-item layout and a unique firmware
induced state change, not hardcoded global indices. Capture is read-only apart
from transient ROM injection/reset; the parent archive is hashed unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from check_mame_h8_peripherals import run_mame

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / 'tests/mame_h8_sync_timer_oracle.lua'
PERIOD = 10**18 // 16934400
LIMIT = 96


def firmware(channel, stopped):
    words = []
    base = 0xffb0 + channel * 8
    for address, value in [(base, 0x80), (base+1, 255 if stopped else 7),
                           (base+2, 0x10)] + ([(base+2, 0)] if stopped else []):
        words.extend((0xf800 | value, 0x6a88, address))
    loop = 0x100 + len(words)*2
    words.extend((0x6aa8, 0x0008, 0x0000))
    words.append(0x4000 | ((loop - (0x100 + (len(words)+1)*2)) & 255))
    return words


def parse_time(seconds, attoseconds):
    return dict(seconds=int(seconds), attoseconds=int(attoseconds))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mame', default='/opt/homebrew/bin/mame')
    parser.add_argument('--rompath', type=Path, default=ROOT/'roms')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    before_rom, before_template = sha(args.rompath/'soulclbr.zip'), sha(TEMPLATE)
    version = subprocess.check_output([args.mame, '-version'], text=True, timeout=10).strip()
    executable = Path(shutil.which(args.mame) or args.mame).resolve()
    captures, logs = [], []
    with tempfile.TemporaryDirectory(prefix='mame-h8-sync-') as temporary:
        root = Path(temporary)
        for channel in (0, 1):
            for stopped in (False, True):
                work = root/f'{channel}-{int(stopped)}'
                work.mkdir()
                words = firmware(channel, stopped)
                script = work/'probe.lua'
                script.write_text(TEMPLATE.read_text().replace('CHANNEL', str(channel))
                                  .replace('INJECT_WORDS', ', '.join(hex(w) for w in words))
                                  .replace('MAXOBS', str(LIMIT)))
                out, code, command, timeout = run_mame(args.mame, args.rompath.resolve(), script, work)
                logs.append(f'channel={channel} stopped={int(stopped)}\n'+out)
                args.log.write_text('\n'.join(logs))
                assert code == 0 and not timeout and 'LUA ERROR' not in out, out[-4000:]
                resets = [parse_time(s, a) for _, s, a in re.findall(
                    r'H8SYNC_RESET n=(\d+) time=(\d+):(\d+)', out)]
                assert resets == [dict(seconds=0, attoseconds=0)], resets
                loaded = re.search(r'H8SYNC_FIRMWARE words=([0-9a-f,]+)', out)
                assert loaded and [int(w, 16) for w in loaded[1].split(',')] == words
                rows = []
                for n,s,a,armed,es,ea,ss,sa,event in re.findall(
                        r'H8SYNC_OBS n=(\d+) time=(\d+):(\d+) armed=(\d+) '
                        r'expire=(\d+):(\d+) start=(\d+):(\d+) clock_event=(\d+)', out):
                    time = parse_time(s, a)
                    absolute = time['seconds']*10**18 + time['attoseconds']
                    assert absolute % PERIOD == 0
                    rows.append(dict(index=int(n), time=time, cycle=absolute//PERIOD,
                                     armed=bool(int(armed)), expire=parse_time(es,ea),
                                     start=parse_time(ss,sa), clock_event=int(event)))
                assert len(rows) == LIMIT, out[-2000:]
                ident = re.search(r'H8SYNC_IDENT index=(\d+) groups=(\d+) registry_items=(\d+)', out)
                assert ident
                if stopped:
                    assert all(r['clock_event']==0 for r in rows)
                    assert rows[0]['armed'] and not rows[-1]['armed'], rows
                else:
                    # Without servicing RDRF, source synchronous RX eventually
                    # stops on overrun. Keep that completion and stale pulse.
                    assert rows[0]['armed'] and any(r['clock_event'] for r in rows)
                    assert len({(r['expire']['seconds'], r['expire']['attoseconds'])
                                for r in rows if r['armed']}) > 1
                captures.append(dict(channel=channel, stopped=stopped, firmware_words=words,
                                     returncode=code, timed_out=timeout, command=command,
                                     resets=resets, timer_identification=dict(index=int(ident[1]),
                                     candidate_groups=int(ident[2]), registry_items=int(ident[3]),
                                     rule='unique finite one-shot with nonzero start time before first marker'),
                                     rows=rows))
                print(f'SCI{channel} stopped={int(stopped)}: {len(rows)} actual timer states', flush=True)
    evidence = dict(schema='pcsx-aow.mame-h8-sync-timers.v1', mame_version=version,
                    mame_executable=str(executable), mame_sha256=sha(executable),
                    installed_binary_source_match='unverified',
                    source_revision_pin='deb2f54fe7300e7a192690318beb860159bfd52a',
                    source='schedule.cpp emu_timer::register_save, save.h attotime specialization, h8_sci.cpp',
                    h8_hz=16934400, period_as=PERIOD, captures=captures,
                    parent_rom_before=before_rom, parent_rom_after=sha(args.rompath/'soulclbr.zip'),
                    template_before=before_template, template_after=sha(TEMPLATE))
    assert evidence['parent_rom_after']==before_rom and evidence['template_after']==before_template
    args.output.write_text(json.dumps(evidence, indent=2)+'\n')
    print('MAME H8 SCI global timer capture PASS')


if __name__ == '__main__':
    main()
