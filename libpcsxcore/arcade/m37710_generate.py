#!/usr/bin/env python3
"""Regenerate BSD-3-Clause MAME M37710 instruction sources (R. Belmont/Karl Stenerud)."""
from pathlib import Path
import argparse,re
p=argparse.ArgumentParser(); p.add_argument('source',nargs='?',default='../mamedev/src/devices/cpu/m37710'); a=p.parse_args()
s=Path(a.source); d=Path(__file__).resolve().parent
h=(s/'m37710.h').read_text(); c=(s/'m37710.cpp').read_text()
notice='// Generated from MAME; license:BSD-3-Clause\n// copyright-holders:R. Belmont, Karl Stenerud\n'
def out(n,t): (d/n).write_text(notice+t)
out('m37710_enums.inc',h[h.index('enum\n'):h.index('// internal ROM region')])
fields=h[h.index('\tuint32_t m_a;'):h.index('\tmemory_access<24')]
methods=h[h.index('\t// Statics'):h.index('\n};',h.index('\t// Statics'))]
methods=re.sub(r'\tTIMER_CALLBACK_MEMBER\([^;]+;\n','',methods)
out('m37710_declarations.inc',fields+'\nuint32_t m_stopped;\n'+methods)
cm=(s/'m37710cm.h').read_text().replace('#include "m37710.h"','')
cm=cm.replace('debugger_instruction_hook(x)','instruction_hook(x)')
cm=cm.replace('m_program.read_byte(addr)','bus_read8(addr)').replace('m_program.write_byte(addr,data)','bus_write8(addr,data)').replace('m_cache.read_byte(A)','bus_read8(A)').replace('m_program.read_word_unaligned(addr)','bus_read16(addr)').replace('m_program.write_word_unaligned(addr,data)','bus_write16(addr,data)').replace('m_cache.read_word(A)','bus_read16(A)')
out('m37710_macros.inc',cm)
out('m37710_inline.inc',(s/'m37710il.h').read_text())
op=(s/'m37710op.h').read_text()
op=op.replace('#undef TABLE_FUNCTION','#undef TABLE_FUNCTION\n#undef TABLE_OPCODES2\n#undef TABLE_OPCODES3')
op=op.replace('logerror("error M37710: UNIMPLEMENTED OPCODE!  K=%x PC=%x\\n", REG_PG, REG_PPC);','illegal_opcode(); CLK(1);')
op=op.replace('debugger_wait_hook();',';')
out('m37710_opcodes.inc',op)
# Source-defined tables and common interrupt/width helpers, independent of MAME infrastructure.
common=c[c.index('const m37710_cpu_device::opcode_func *const'):c.index('/* external functions */')]
common=common.replace('thispri > m_ipl','uint32_t(thispri) > m_ipl')
common=common.replace('standard_irq_callback(wantedIRQ, REG_PG | REG_PC);','irq_ack(wantedIRQ);')
common+=c[c.index('void m37710_cpu_device::m37710i_set_execution_mode'):c.index('/* ======================================================================== */\n/* ============================== END OF FILE')]
out('m37710_common.inc',common)
out('m37710_vectors.inc',c[c.index('const int m37710_cpu_device::m37710_irq_vectors'):c.index('// M37710 internal peripherals')])
print('Generated full four-mode M37710 ISA, vectors and interrupt helpers')
