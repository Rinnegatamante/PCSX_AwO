#!/usr/bin/env python3
"""Hottest instructions of one symbol from a callgrind file written with
--dump-instr=yes --dump-line=no --compress-pos=no (see cg_by_symbol.py), with
the disassembly next to each address.

  cg_hot_instr.py CALLGRIND_OUT NM_OUTPUT OBJDUMP_OUTPUT SYMBOL [TOP]"""
import bisect, collections, re, sys

cg, nmf, dis, sym = sys.argv[1:5]
top = int(sys.argv[5]) if len(sys.argv) > 5 else 60
syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in open(nmf))
              if len(p) == 3 and p[1] in 'tTwW')
start = [a for a, n in syms if n == sym][0]
end = syms[[a for a, _ in syms].index(start) + 1][0]
asm = {}
for line in open(dis):
    m = re.match(r'\s*([0-9a-f]+):\s+(?:[0-9a-f]{8}|[0-9a-f]{4} [0-9a-f]{4})\s+(.*)', line)
    if m:
        asm[int(m.group(1), 16)] = m.group(2).strip()
events, cost, skip = None, collections.defaultdict(lambda: None), False
for line in open(cg):
    if line.startswith('events:'):
        events = line.split()[1:]
    elif line.startswith('calls='):
        skip = True
    elif line[:1] in '0123456789':
        if skip:
            skip = False
            continue
        p = line.split()
        a = int(p[0], 16)
        if start <= a < end:
            v = [int(x) for x in p[1:]]
            c = cost[a]
            if c is None:
                cost[a] = v + [0] * (len(events) - len(v))
            else:
                for k, x in enumerate(v):
                    c[k] += x
tot = sum(c[0] for c in cost.values())
print('%s: %d Ir, %d instructions executed' % (sym, tot, len(cost)))
print('%10s %8s %8s  %s' % ('Ir', events[4] if len(events) > 4 else '', events[5] if len(events) > 5 else '', 'instruction'))
for a in sorted(cost):
    c = cost[a]
    if c[0] * top >= tot or (len(c) > 4 and (c[4] or c[5])):
        print('%10d %8d %8d  %x: %s' % (c[0], c[4] if len(c) > 4 else 0, c[5] if len(c) > 5 else 0, a, asm.get(a, '?')))
