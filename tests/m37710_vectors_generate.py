#!/usr/bin/env python3
"""Independent width-limited arithmetic oracle for MAME M37710 binary ALU equations.
Writes fixed fixtures; does not import or execute the port or its instruction macros.
"""
from pathlib import Path
ops={0x69:'adc',0xe9:'sbc',0xc9:'cmp',0x29:'and',0x09:'or',0x49:'xor'}
values=[0,1,0x7f,0x80,0xff,0x100,0x7fff,0x8000,0xffff,0x1234,0x5678,0xa55a]
rows=[]
for mode in range(4):
    bits=8 if mode&2 else 16; mask=(1<<bits)-1; sign=1<<(bits-1)
    for prefix in [0,0x42]:
        for opcode,op in ops.items():
            for i,lhs in enumerate(values):
                rhs=values[(i*5+3)%len(values)]; carry=i&1; overflow=(i>>1)&1
                low=lhs&mask; right=rhs&mask; c=carry; v=overflow
                if op=='adc':
                    total=low+right+carry; result=total&mask;c=int(total>mask)
                    v=int(bool((~(low^right)&(low^result))&sign))
                elif op in ['sbc','cmp']:
                    total=low-right-(1-carry if op=='sbc' else 0); result=total&mask;c=int(total>=0)
                    if op=='sbc':v=int(bool(((low^right)&(low^result))&sign))
                else:result={'and':low&right,'or':low|right,'xor':low^right}[op]
                flags=(0x80 if result&sign else 0)|(v<<6)|(2 if result==0 else 0)|c
                final=lhs if op=='cmp' else (lhs&~mask)|result
                ps=4|((mode&2)<<4)|((mode&1)<<4)|carry|(overflow<<6)
                rows.append((mode,prefix,opcode,lhs,rhs,ps,final,flags,2 if bits==8 else 3))
out=Path(__file__).with_name('m37710_vectors.inc')
out.write_text('// Fixed independently calculated binary ALU vectors. Regenerate with m37710_vectors_generate.py.\n'+
 'static const AluVector alu_vectors[] = {\n'+''.join('    {'+', '.join(hex(x) for x in r)+'},\n' for r in rows)+'};\n')
print(f'Wrote {len(rows)} ALU vectors')

# Opcode enumeration is source-derived coverage data, independent of the ALU oracle above.
import re, hashlib
source=Path('../mamedev/src/devices/cpu/m37710/m37710op.h')
if source.exists():
    text=source.read_text(); entries=[]
    for table,prefix in [('TABLE_OPCODES',0),('TABLE_OPCODES2',0x42),('TABLE_OPCODES3',0x89)]:
        body=re.search(r'\b'+table+r'\s*=.*?\{(.*?)\};',text,re.S).group(1)
        slots=re.findall(r'O\(([0-9a-f]+)\)',body)
        assert len(slots)==256
        entries.extend((prefix,i) for i,v in enumerate(slots) if v!='200')
    Path(__file__).with_name('m37710_smoke.inc').write_text(
        '// Valid dispatch slots from upstream MAME m37710op.h.\n// Source SHA256: '+hashlib.sha256(source.read_bytes()).hexdigest()+'\n'+
        'static const uint8_t smoke_opcodes[][2] = {\n'+''.join(f'    {{0x{p:02x},0x{c:02x}}},\n' for p,c in entries)+'};\n')
    print(f'Wrote {len(entries)} valid opcode dispatch slots')
