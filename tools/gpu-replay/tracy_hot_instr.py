#!/usr/bin/env python3
"""Self samples per instruction of one function from a vita-tracy samples
dump (tsamples), next to its disassembly: for assembly functions generated
by macros, where source-line attribution collapses to one line.

  tracy_hot_instr.py SAMPLES.txt ELF FUNCTION [MIN_PERCENT]"""
import collections, os, re, subprocess, sys

txt, elf, func = sys.argv[1:4]
minp = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
tool = os.environ.get('VITASDK', '/usr/local/vitasdk') + '/bin/arm-vita-eabi-'
syms = []
for line in subprocess.run([tool + 'nm', '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
start = next(a for a, n in syms if n == func)
end = next(a for a, n in syms if a > start)
counts = collections.Counter()
for line in open(txt):
    if line.startswith('S\t'):
        pc = int(line.split('\t')[4], 16) & ~1
        if start <= pc < end:
            counts[pc] += 1
total = sum(counts.values())
dis = subprocess.run([tool + 'objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % start,
                      '--stop-address=%#x' % end, elf], capture_output=True, text=True).stdout
print('%s: %d samples' % (func, total))
for line in dis.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
    if not m:
        continue
    a = int(m.group(1), 16)
    c = counts.get(a, 0)
    mark = '%5.1f%%' % (100.0 * c / total) if c and 100.0 * c / total >= minp else '      '
    print('%s  %x: %s' % (mark, a, m.group(2)))
