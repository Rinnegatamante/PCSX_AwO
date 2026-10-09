#!/usr/bin/env python3
"""Self cost per symbol from a callgrind file written with --dump-instr=yes
--dump-line=no --compress-pos=no --compress-strings=no, attributing each
instruction to the symbol containing it (nm -n). The NEON assembly's symbols
carry no .size, so callgrind cannot name them itself.

  cg_by_symbol.py CALLGRIND_OUT NM_OUTPUT [--events]"""
import bisect, collections, sys

def main():
    cg, nmf = sys.argv[1], sys.argv[2]
    syms = []
    for line in open(nmf):
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwW':
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    addrs = [a for a, _ in syms]
    events, cost = None, collections.defaultdict(lambda: None)
    skip = False
    obj_main = True
    for line in open(cg):
        if line.startswith('events:'):
            events = line.split()[1:]
            continue
        if line.startswith('ob='):
            obj_main = 'gpu-replay' in line
            continue
        if line.startswith('calls='):
            skip = True
            continue
        if not line[:1] in '0123456789':
            continue
        if skip:
            skip = False
            continue
        p = line.split()
        addr = int(p[0], 16)
        vals = [int(v) for v in p[1:]]
        if obj_main:
            i = bisect.bisect_right(addrs, addr) - 1
            name = syms[i][1] if i >= 0 else '?'
        else:
            name = '(libc/other)'
        c = cost[name]
        if c is None:
            c = cost[name] = [0] * len(events)
        for k, v in enumerate(vals):
            c[k] += v
    total = [sum(c[k] for c in cost.values() if c) for k in range(len(events))]
    print('total', ' '.join('%s=%d' % (e, t) for e, t in zip(events, total)))
    hdr = '%-60s' % 'symbol' + ''.join('%14s' % e for e in events)
    print(hdr)
    for name, c in sorted(cost.items(), key=lambda kv: -kv[1][0])[:int(sys.argv[3]) if len(sys.argv) > 3 else 40]:
        print('%-60s' % name[:60] + ''.join('%14d' % v for v in c) + '  %5.1f%%' % (100.0 * c[0] / total[0]))

main()
