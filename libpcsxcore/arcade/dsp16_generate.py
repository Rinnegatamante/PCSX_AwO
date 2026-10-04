#!/usr/bin/env python3
"""Extract the complete pinned MAME DSP16 interpreter, without its framework/DRC.
BSD-3-Clause; upstream copyright Vas Crabb. Regenerate with MAME source directory.
"""
import pathlib,re,sys
src=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else '../mamedev/src/devices/cpu/dsp16')
out=pathlib.Path(__file__).resolve().parent
h=(src/'dsp16.h').read_text(); c=(src/'dsp16.cpp').read_text()
def section(s,start,end): return s[s.index(start):s.index(end)]
common=section(h,'\t// masks for registers', '\t// recompiler helpers')
decls=section(h,'\t// instruction execution','\t// interrupt callbacks')
fields=section(h,'\t// execution state\n\tcache       m_cache_mode','\n};\n\n\nclass dsp16_device')
core=(src/'dsp16core.h').read_text()
core=section(core,'class dsp16_device_base::core_state','inline void dsp16_device::core_destructer')
core=core.replace('\tvoid register_save_items(device_t &host);','')
core=core.replace('void decrement_icount() { --icount; }','void decrement_icount() { --icount; ++cycles; }').replace('int     icount = 0;','int     icount = 0;\n\tuint64_t cycles = 0;')
ipp=(src/'dsp16core.ipp').read_text()
ipp=section(ipp,'inline u16 &dsp16_device_base::core_state::yaau_postmodify_r', '#endif')
body=section(c,'u16 dsp16_device_base::pio_r()', '/***********************************************************************\n    construction/destruction')
body+=section(c,'void dsp16_device_base::device_reset()', '/***********************************************************************\n    device_execute_interface')
body+=section(c,'void dsp16_device_base::execute_set_input(', '/***********************************************************************\n    device_memory_interface')
body+=section(c,'template <bool Debugger, bool Caching> inline void dsp16_device_base::execute_some_rom()', '/***************************************************************************\n    DSP16 SPECIALISATION')
body=body.replace('\tcpu_device::device_reset();','')
body=body.replace('m_spaces[AS_PROGRAM]->read_word(', 'program_read(').replace('m_pcache.read_word(', 'program_read(')
body=body.replace('m_spaces[AS_DATA]->read_word(r)', 'm_workram[r & m_workram_mask]').replace('m_spaces[AS_DATA]->write_word(r, value)', 'm_workram[r & m_workram_mask] = value').replace('m_spaces[AS_DATA]->write_word(r, m_core->dau_temp)', 'm_workram[r & m_workram_mask] = m_core->dau_temp')
# Unsupported upstream zero-return placeholders must stop, not fabricate input.
body=re.sub(r'logerror\(("DSP16: unimplemented [^;]+)\);',r'throw emu_fatalerror(\1);',body)
body=body.replace('m_pio_pioc = (m_pio_pioc & 0x801fU)', 'if (BIT(value, 10)) throw emu_fatalerror("DSP16: PIO S/C mode is unimplemented\\n");\n\tm_pio_pioc = (m_pio_pioc & 0x801fU)')
body=body.replace('// FIXME: implement enough to put something here','if (!unsupported_rx_warned) { logerror("DSP16: serial input frame ignored; RX data is unimplemented\\n"); unsupported_rx_warned = true; }')
# Count each initiated ISA instruction, irrespective of debugger predicate grouping.
body=body.replace('case phase::OP1:\n', 'case phase::OP1:\n\t\t\t++instructions;\n')
body=body.replace('static constexpr unsigned DIVIDERS', '[[maybe_unused]] static constexpr unsigned DIVIDERS')
body=body.replace('assert(', 'DSP16_REQUIRE(')
ipp=ipp.replace('assert(', 'DSP16_REQUIRE(')
(out/'dsp16_engine.inc').write_text('// Generated from MAME: BSD-3-Clause, copyright Vas Crabb.\n'+body)
(out/'dsp16_core.inc').write_text('// Generated from MAME: BSD-3-Clause, copyright Vas Crabb.\n'+core+ipp)
(out/'dsp16_declarations.inc').write_text('// Generated from MAME: BSD-3-Clause, copyright Vas Crabb.\n'+common+decls+fields)
