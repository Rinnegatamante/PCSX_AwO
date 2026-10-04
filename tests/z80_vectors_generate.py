#!/usr/bin/env python3
"""Independent byte/flag equations for the NMOS Z80 behavior in MAME z80.cpp.
No port code or generated execution engine is imported or executed.
"""
from pathlib import Path
import hashlib
rows=[]
def parity(v):return int(bin(v&255).count('1')%2==0)
def szxy(r):return (r&0xa8)|(0x40 if r==0 else 0)
values=[0,1,7,8,15,16,31,32,63,64,0x7f,0x80,0xfe,0xff]
for opcode,op in [(0xc6,'add'),(0xce,'adc'),(0xd6,'sub'),(0xde,'sbc'),(0xe6,'and'),(0xee,'xor'),(0xf6,'or'),(0xfe,'cp')]:
 for a in values:
  for b in values:
   for carry in [0,1]:
    n=0;h=0;c=0;v=0
    if op in ['add','adc']:
     total=a+b+(carry if op=='adc' else 0);r=total&255;c=int(total>255);h=(a^b^r)&16;v=int(bool(~(a^b)&(a^r)&128))
    elif op in ['sub','sbc','cp']:
     total=a-b-(carry if op=='sbc' else 0);r=total&255;c=int(total<0);h=(a^b^r)&16;v=int(bool((a^b)&(a^r)&128));n=2
    else:
     r={'and':a&b,'xor':a^b,'or':a|b}[op];v=parity(r);h=16 if op=='and' else 0
    flags=szxy(r)|h|(v<<2)|n|c
    if op=='cp':flags=(flags&~0x28)|(b&0x28)
    rows.append((0,opcode,a,b,carry,a if op=='cp' else r,flags,7))
for opcode in [0x3c,0x3d]:
 for a in range(256):
  for carry in [0,1]:
   r=(a+(1 if opcode==0x3c else -1))&255
   half=16 if ((a&15)==(15 if opcode==0x3c else 0)) else 0
   overflow=4 if a==(0x7f if opcode==0x3c else 0x80) else 0
   rows.append((0,opcode,a,0,carry,r,szxy(r)|half|overflow|(2 if opcode==0x3d else 0)|carry,4))
for a in range(256):
 for initial in range(8):
  c=initial&1;n=(initial>>1)&1;h=(initial>>2)&1
  correction=(6 if h or (a&15)>9 else 0)+(0x60 if c or a>0x99 else 0)
  r=(a+(-correction if n else correction))&255
  flags=szxy(r)|((a^r)&16)|(parity(r)<<2)|(n<<1)|int(c or a>0x99)
  rows.append((0,0x27,a,0,c|(n<<1)|(h<<4),r,flags,4))
for group in range(8):
 for value in range(256):
  for carry in [0,1]:
   if group==0:r=((value<<1)|(value>>7))&255;c=value>>7
   elif group==1:r=(value>>1)|((value&1)<<7);c=value&1
   elif group==2:r=((value<<1)|carry)&255;c=value>>7
   elif group==3:r=(value>>1)|(carry<<7);c=value&1
   elif group==4:r=(value<<1)&255;c=value>>7
   elif group==5:r=(value>>1)|(value&128);c=value&1
   elif group==6:r=((value<<1)|1)&255;c=value>>7
   else:r=value>>1;c=value&1
   rows.append((0xcb,group*8,value,0,carry,r,szxy(r)|(parity(r)<<2)|c,8))
src=Path('../mamedev/src/devices/cpu/z80/z80.cpp')
notice='// Fixed independently calculated Z80 byte/flag vectors. Regenerate with z80_vectors_generate.py.\n'
if src.exists():notice+='// MAME equation-source SHA256: '+hashlib.sha256(src.read_bytes()).hexdigest()+'\n'
Path(__file__).with_name('z80_vectors.inc').write_text(notice+'static const AluVector alu_vectors[] = {\n'+''.join('    {'+', '.join(hex(x) for x in row)+'},\n' for row in rows)+'};\n')
print('Generated',len(rows),'independent arithmetic/rotate/DAA vectors')
wide=[]
values16=[0,1,0xf,0x10,0xff,0x100,0xfff,0x1000,0x7fff,0x8000,0xffff,0x1234]
for prefix,opcode,op,cycles in [(0,9,'add',11),(0xed,0x4a,'adc',15),(0xed,0x42,'sbc',15)]:
 for a in values16:
  for b in values16:
   for carry in [0,1]:
    initial=0xc4|carry
    if op=='sbc':
     total=a-b-carry;r=total&65535;c=int(total<0);v=int(bool((a^b)&(a^r)&0x8000));n=2
    else:
     total=a+b+(carry if op=='adc' else 0);r=total&65535;c=int(total>65535);v=int(bool(~(a^b)&(a^r)&0x8000));n=0
    flags=((r>>8)&0x28)|(((a^b^r)&0x1000)>>8)|n|c
    flags|=(initial&0xc4) if op=='add' else ((0x80 if r&0x8000 else 0)|(0x40 if not r else 0)|(v<<2))
    wide.append((prefix,opcode,a,b,initial,r,flags,cycles))
with Path(__file__).with_name('z80_vectors.inc').open('a') as f:
 f.write('static const WideVector wide_vectors[] = {\n'+''.join('    {'+', '.join(hex(x) for x in row)+'},\n' for row in wide)+'};\n')
print('Generated',len(wide),'independent 16-bit ADD/ADC/SBC vectors')
