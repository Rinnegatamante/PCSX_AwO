#!/usr/bin/env python3
"""Standalone adaptation of MAME z80make.py/z80.lst (BSD-3-Clause).
Copyright Juergen Buchmueller, Andrei I. Holub / Andre I. Holub.
"""
from pathlib import Path
import argparse, importlib.util, tempfile, io, re
p=argparse.ArgumentParser();p.add_argument('source',nargs='?',default='../mamedev/src/devices/cpu/z80');a=p.parse_args()
s=Path(a.source);d=Path(__file__).resolve().parent
notice='// Generated from MAME; license:BSD-3-Clause\n// copyright-holders:Juergen Buchmueller, Andrei I. Holub\n'
def out(n,t): (d/n).write_text(notice+t)
h=(s/'z80.h').read_text();cpp=(s/'z80.cpp').read_text()
methods=h[h.index('\tvoid illegal_1();'):h.index('\t// address spaces')].replace('virtual ','')
fields=h[h.index('\tstatic constexpr u8 SA_BUSREQ'):h.index('\n};',h.index('\tstatic constexpr u8 SA_BUSREQ'))]
out('z80_declarations.inc',methods+fields)
out('z80_macros.inc',(s/'z80.inc').read_text())
helpers=cpp[cpp.index('u8 z80_device::get_f()'):cpp.index('/****************************************************************************\n * Processor initialization')]
helpers=helpers.replace('m_data.read_interruptible(addr)','memory_read(addr)').replace('m_data.write_interruptible(addr, value)','memory_write(addr, value)').replace('m_opcodes.read_byte(PC)','memory_opcode(PC)').replace('m_args.read_byte(PC)','memory_read(PC)')
# Undefined ED bytes are physical undocumented NOPs; ignored DD/FD prefixes execute base opcode.
helpers=re.sub(r'void z80_device::illegal_1\(\).*?(?=void z80_device::illegal_2)', 'void z80_device::illegal_1() { undocumented_count++; }\n\n',helpers,flags=re.S)
helpers=re.sub(r'void z80_device::illegal_2\(\).*', 'void z80_device::illegal_2() { undocumented_count++; }\n',helpers,flags=re.S)
out('z80_helpers.inc',helpers)
# Keep original primary input sources for regeneration and license review.
(d/'z80_mame.lst').write_text((s/'z80.lst').read_text())
(d/'z80_mame_make.py').write_bytes((s/'z80make.py').read_bytes())
spec=importlib.util.spec_from_file_location('mame_z80make',s/'z80make.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
lst=(s/'z80.lst').read_text()
lst=re.sub(r'macro   irqfetch\n.*?(?=macro   t6a84:irqfetch)', 'macro   irqfetch\n\tm_tmp_irq_vector = irq_vector();\n\n',lst,flags=re.S)
lst=lst.replace('macro   take_nmi\n','macro   take_nmi\n\tnmis++;\n')
with tempfile.NamedTemporaryFile('w',suffix='.lst') as tmp:
    tmp.write(lst);tmp.flush();ops=mod.OpcodeList(None,tmp.name)
    # Hooks count completed instructions and optionally stop before the next opcode fetch.
    for prefix,entries in ops.opcode_info.items():
        for op in entries:
            if prefix!='ff':
                op.source.append(mod.IndStr('\tif (instruction_end()) return;'))
            else:
                # Debugger step of a HALT machine cycle must return rather than spin forever.
                for i,line in enumerate(op.source):
                    if line.line()=='PC--;':
                        op.source.insert(i+1,mod.IndStr('\t\tif (stop_after_instruction) { m_ref=0xffff00; return; }'))
                        break
    output=io.StringIO();ops.save_exec(output);out('z80_execution.inc',output.getvalue())
counts={k:len(v) for k,v in ops.opcode_info.items()};print('Generated Z80 bus-cycle engine:',counts)
