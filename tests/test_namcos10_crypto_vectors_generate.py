#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Equations derived from MAME ns10crypt.cpp (Andreas Naive).
"""Generate fixed vectors using an independent Python implementation of MAME equations."""
import hashlib
from pathlib import Path
import re
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'libpcsxcore/arcade'))
from namcos10_crypto_generate import profiles,balanced
MASK64=(1<<64)-1
TABLE=bytes((0x04,0x9d))
WORDS=[0,0xffff,0x1234,0x8000,0x0001,0x5555,0xaaaa,0xfedc,0x3210,0xbabe,0x1000,0x7fff,0x2468,0x1357,0xabcd,0x9876]*2

def swap(word,order):
    return sum(((word>>b)&1)<<(len(order)-1-i) for i,b in enumerate(order))
def parity(word):
    return bin(word).count('1')%2

def evaluate(expr,cipher,plain,bit=0):
    expr=re.sub(r'(0x[0-9a-fA-F]+|\d+)(?:ULL|UL|LL)',r'\1',expr)
    return eval(' '.join(expr.split()),{'__builtins__':{}},{'previous_cipherwords':cipher,'previous_plainwords':plain,'previous_masks':cipher^plain,'nonlinear_bit':bit,'gf2_reduce':parity})
def type1(iv,words):
    sbox=[16,16,16,0,4,9,16,16,16,8,16,1,16,9,16,5];mask=sbox[iv];out=[]
    for pos,c in enumerate(words):
        d=mask^swap(c,[9,13,15,7,14,8,6,10,11,12,3,5,0,1,4,2]);out.append(d)
        b=lambda n:(c>>n)&1;k=lambda n:((pos&255)>>n)&1
        terms={15:k(4),14:b(2)^b(5),13:b(0),12:b(4)|b(5),11:k(0),10:b(6),8:b(4)&k(1),6:k(3),5:b(3)|k(7),4:b(2)^k(3),3:k(2),2:b(7)&k(7),1:k(5),0:b(7)|k(1)}
        nbs=sum(v<<n for n,v in terms.items())
        mask=nbs^swap(c,[6,11,3,1,13,5,15,10,2,9,8,4,0,12,7,14])^swap(d,[9,7,5,2,14,4,13,8,0,15,10,1,3,6,12,11])^0xecbe
    return out

def type2(logic,kind,iv,words):
    arrays=list(re.finditer(r'\{\s*((?:0x[\da-fA-F]+ULL\s*,?\s*){16})\}',logic))
    assert len(arrays)==2
    e,d=([int(n[:-3],16) for n in re.findall(r'0x[\da-fA-F]+ULL',a)] for a in (m[1] for m in arrays))
    stop=arrays[1].end()
    x=int(re.search(r',\s*(0x[\da-fA-F]+)',logic[stop:])[1],16)
    nl=re.search(r'\[\]\s*\([^)]*\)\s*->\s*uint16_t\s*\{',logic)
    nlbody=balanced(logic,nl.end()-1);expr=re.search(r'return\s+(.*);',nlbody,re.S)[1]
    ivtable=re.search(r'values\[16\]\s*\{([^}]*)\}',logic)
    if ivtable:c=int(ivtable[1].split(',')[iv].strip(),0)
    else:
        sbox=[0,12,13,6,2,4,9,8,11,1,7,15,10,5,14,3]
        c=swap(sbox[iv],[3,16,16,2,1,16,16,0,16,16,16,16,16,16,16,16])
    p=0;mask=0;out=[]
    for index,word in enumerate(words):
        plain=word^mask;out.append(plain)
        c=((c<<16)|word)&MASK64;p=((p<<16)|plain)&MASK64
        mask=x^sum((parity(em&c)^parity(dm&p))<<i for i,(em,dm) in enumerate(zip(e,d)))
        bit=(TABLE[(index%16)//8]>>(7-index%8))&1
        mask^=evaluate(expr,c,p,bit if kind==3 else 0)
        mask&=65535
    return out

def main():
    source=Path(sys.argv[1]).read_text();rows=profiles(source)
    text='// Fixed vectors from independent Python MAME-equation reference.\n// MAME namcos10.cpp SHA256: '+hashlib.sha256(source.encode()).hexdigest()+'\n'
    text+='static const uint16_t cipherwords[]={'+','.join(f'0x{x:04x}' for x in WORDS)+'};\n'
    text+='struct Vector {const char *name;unsigned iv;uint16_t plain[32];};\nstatic const Vector vectors[]={\n'
    count=0
    for name,kind,perm,logic,known,xor in rows:
        if not kind:continue
        for iv in range(16):
            if not known&(1<<iv):continue
            out=type1(iv,WORDS) if kind==1 else type2(logic,kind,iv,WORDS)
            text+='{"'+name+'",'+str(iv)+',{'+','.join(f'0x{x:04x}' for x in out)+'}},\n';count+=1
    text+='};\nstatic const uint16_t type1_wrap[]={'+','.join(f'0x{x:04x}' for x in type1(3,WORDS*10))+'};\n'
    text+='struct PassiveVector {const char *name;uint16_t result;};\nstatic const PassiveVector passive_vectors[]={'+','.join('{"'+n+'",0x%04x}'%swap(0x1357^xor,[int(t,0) for t in perm.split(',')]) for n,k,perm,l,known,xor in rows)+'};\n'
    Path(__file__).with_name('test_namcos10_crypto_vectors.inc').write_text(text)
    print('Fixed reference vectors:',count,'streams plus type1counterwrap and',len(rows),'passiveprofiles')
if __name__=='__main__':main()
