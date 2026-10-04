// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/***************************************************************************

    tms57002.cpp

    TMS57002 "DASP" emulator.

***************************************************************************/

#include "tms57002_core.h"
#include <cmath>


tms57002_device::tms57002_device(data_read_callback read, data_write_callback write,
                                   line_callback dready, line_callback pc0,
                                   line_callback empty)
    : m_dready_callback(std::move(dready)), m_pc0_callback(std::move(pc0)),
      m_empty_callback(std::move(empty)), m_data_read(std::move(read)),
      m_data_write(std::move(write))
{
    reset();
}

void tms57002_device::execute_cycles(u32 cycles)
{
    icount = static_cast<int>(cycles);
    m_slice_budget = cycles;
    execute_run();
}

void tms57002_device::program_write(u8 address, u32 value)
{
    program_ram[address] = value;
    cache_flush();
    update_pc0();
}

u32 tms57002_device::dmem_read(unsigned bank, unsigned address) const
{
    if (bank == 0 && address < 256) return dmem0[address];
    if (bank == 1 && address < 32) return dmem1[address];
    return 0;
}

void tms57002_device::pload_w(int state)
{
	u8 olds = sti;
	if (state)
		sti &= ~IN_PLOAD;
	else
		sti |= IN_PLOAD;
	if (olds ^ sti)
	{
		if (sti & IN_PLOAD)
		{
			hidx = 0;
			pc = 0;
			ca = 0;
			sti &= ~(SU_MASK);
		}
	}
}

void tms57002_device::cload_w(int state)
{
	u8 olds = sti;
	if (state)
		sti &= ~IN_CLOAD;
	else
		sti |= IN_CLOAD;
	if (olds ^ sti)
	{
		if (sti & IN_CLOAD)
		{
			hidx = 0;
			//ca = 0; // Seems extremely dubious
		}
	}
}

void tms57002_device::reset()
{
	sti = (sti & ~(SU_MASK|S_READ|S_WRITE|S_BRANCH|S_HOST|S_UPDATE)) | (SU_ST0|S_IDLE);
	pc = 0;
	ca = 0;
	hidx = 0;
	id = 0;
	ba0 = 0;
	ba1 = 0;
	sa = 0;
	rptc = 0;
	rptc_next = 0;
	update_counter_tail = 0;
	update_counter_head = 0;
	st0 &= ~(ST0_INCS | ST0_DIRI | ST0_FI | ST0_SIM | ST0_PLRI |
			ST0_PBCI | ST0_DIRO | ST0_FO | ST0_SOM | ST0_PLRO |
			ST0_PBCO | ST0_CNS);
	st1 &= ~(ST1_AOV | ST1_SFAI | ST1_SFAO | ST1_AOVM | ST1_MOVM | ST1_MOV |
			ST1_SFMA | ST1_SFMO | ST1_RND | ST1_CRM | ST1_DBP);
	update_dready();
	update_pc0();
	update_empty();

	xba = 0;
	xoa = 0;
	cache_flush();
}

void tms57002_device::data_w(u8 data)
{
	switch (sti & (IN_PLOAD|IN_CLOAD))
	{
	case 0:
		hidx = 0;
		sti &= ~SU_CVAL;
		break;
	case IN_PLOAD:
		host[hidx++] = data;
		if (hidx >= 3)
		{
			u32 val = (host[0]<<16) | (host[1]<<8) | host[2];
			hidx = 0;

			switch (sti & SU_MASK)
			{
			case SU_ST0:
				st0 = val;
				sti = (sti & ~SU_MASK) | SU_ST1;
				break;
			case SU_ST1:
				st1 = val;
				sti = (sti & ~SU_MASK) | SU_PRG;
				break;
			case SU_PRG:
				program_ram[pc++] = val;
				update_pc0();
				break;
			}
		}
		break;
	case IN_CLOAD:
		if (sti & SU_CVAL)
		{
			host[hidx++] = data;
			if (hidx >= 4)
			{
				u32 val = (host[0]<<24) | (host[1]<<16) | (host[2]<<8) | host[3];
				sti &= ~SU_CVAL;
				update[update_counter_head] = val;
				update_counter_head = (update_counter_head + 1) & 0x0f;
				hidx = 1; // the write shouldn't really happen until CLOAD is high though
				update_empty();
			}
		}
		else
		{
			sa = data;
			hidx = 0;
			sti |= SU_CVAL;
		}

		break;
	case IN_PLOAD|IN_CLOAD:
		host[hidx++] = data;
		if (hidx >= 4)
		{
			u32 val = (host[0]<<24) | (host[1]<<16) | (host[2]<<8) | host[3];
			hidx = 0;
			cmem[ca++] = val;
		}
		break;
	};
}

u8 tms57002_device::data_r()
{
	u8 res;
	if (!(sti & S_HOST))
		return 0xff;

	res = host[hidx];
	hidx++;
	if (hidx == 4)
	{
		hidx = 0;
		sti &= ~S_HOST;
		update_dready();
	}

	return res;
}

int tms57002_device::dready_r() const
{
	return sti & S_HOST ? 0 : 1;
}

void tms57002_device::update_dready()
{
	m_dready_callback(sti & S_HOST ? 0 : 1);
}

int tms57002_device::pc0_r() const
{
	return pc == 0 ? 0 : 1;
}

void tms57002_device::update_pc0()
{
	m_pc0_callback(pc == 0 ? 0 : 1);
}

int tms57002_device::empty_r() const
{
	return (update_counter_head == update_counter_tail);
}

void tms57002_device::update_empty()
{
	m_empty_callback(update_counter_head == update_counter_tail);
}

void tms57002_device::sync_w(int state)
{
	if (sti & (IN_PLOAD /*| IN_CLOAD*/))
		return;

	allow_update = 1;
	pc = 0;
	ca = 0;
	id = 0;
	if (!(st0 & ST0_INCS))
	{
		ba0--;
		ba1++;
	}
	xba = (xba-1) & 0x7ffff;
	st1 &= ~(ST1_AOV | ST1_MOV);
	sti &= ~S_IDLE;
}

void tms57002_device::xm_init()
{
	u32 adr = xoa + xba;
	u32 mask = 0;

	switch (st0 & ST0_M)
	{
	case ST0_M_64K:  mask = 0x0ffff; break;
	case ST0_M_256K: mask = 0x3ffff; break;
	case ST0_M_1M:   mask = 0xfffff; break;
	}
	if (st0 & ST0_WORD)
		adr <<= 2;
	else
		adr <<= 1;

	if (!(st0 & ST0_SEL))
		adr <<= 1;

	xm_adr = adr & mask;
}

inline void tms57002_device::xm_step_read()
{
	u32 adr = xm_adr;
	u8 v = m_data_read ? m_data_read(adr) : 0xff;
	int done;
	if (st0 & ST0_WORD)
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 3) << 3);
			txrd = (txrd & ~(0xff << off)) | (v << off);
			done = off == 0;
		}
		else
		{
			int off = 20 - ((adr & 7) << 2);
			txrd = (txrd & ~(0xf << off)) | ((v & 0xf) << off);
			done = off == 0;
		}
	}
	else
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 1) << 3);
			txrd = (txrd & ~(0xff << off)) | (v << off);
			done = off == 8;
			if (done)
				txrd &= 0xffff00;
		}
		else
		{
			int off = 20 - ((adr & 3) << 2);
			txrd = (txrd & ~(0xf << off)) | ((v & 0xf) << off);
			done = off == 8;
			if (done)
				txrd &= 0xffff00;
		}
	}
	if (done)
	{
		xrd = txrd;
		sti &= ~S_READ;
		xm_adr = 0;
	}
	else
		xm_adr = adr+1;
}

inline void tms57002_device::xm_step_write()
{
	u32 adr = xm_adr;
	u8 v;
	int done;
	if (st0 & ST0_WORD)
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 3) << 3);
			v = xwr >> off;
			done = off == 0;
		}
		else
		{
			int off = 20 - ((adr & 7) << 2);
			v = (xwr >> off) & 0xf;
			done = off == 0;
		}
	}
	else
	{
		if (st0 & ST0_SEL)
		{
			int off = 16 - ((adr & 1) << 3);
			v = xwr >> off;
			done = off == 8;
		}
		else
		{
			int off = 20 - ((adr & 3) << 2);
			v = (xwr >> off) & 0xf;
			done = off == 8;
		}
	}
	if (m_data_write) m_data_write(adr, v);
	if (done)
	{
		sti &= ~S_WRITE;
		xm_adr = 0;
	}
	else
		xm_adr = adr+1;
}

s64 tms57002_device::macc_to_output_0(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::macc_to_output_1(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
		over = true;
	m <<= 2;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::macc_to_output_2(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
		over = true;
	m <<= 4;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::macc_to_output_3(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m >>= 8;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::macc_to_output_0s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::macc_to_output_1s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
		over = true;
	m <<= 2;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::macc_to_output_2s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
		over = true;
	m <<= 4;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::macc_to_output_3s(s64 rounding, u64 rmask)
{
	s64 m = macc_read;
	u64 m1;
	int over = false;

	// Overflow detection and shifting
	m >>= 8;

	m = (m + rounding) & rmask;

	// Second overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
		over = true;

	// Overflow handling
	if (over)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_0()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_1()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_2()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
	{
		st1 |= ST1_MOV;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_3()
{
	return macc_read;
}

s64 tms57002_device::check_macc_overflow_0s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xf800000000000ULL;
	if (m1 && m1 != 0xf800000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_1s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xfe00000000000ULL;
	if (m1 && m1 != 0xfe00000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_2s()
{
	s64 m = macc_read;
	u64 m1;

	// Overflow detection
	m1 = m & 0xff80000000000ULL;
	if (m1 && m1 != 0xff80000000000ULL)
	{
		st1 |= ST1_MOV;
		if (m & 0x8000000000000ULL)
			m = 0xffff800000000000ULL;
		else
			m = 0x00007fffffffffffULL;
	}
	return m;
}

s64 tms57002_device::check_macc_overflow_3s()
{
	return macc_read;
}

u32 tms57002_device::get_cmem(u8 addr)
{
	if (sa == addr && update_counter_head != update_counter_tail)
		sti |= S_UPDATE;

	if (sti & S_UPDATE)
	{
		cmem[addr] = update[update_counter_tail];
		update_counter_tail = (update_counter_tail + 1) & 0x0f;
		update_empty();

		if (update_counter_head == update_counter_tail)
			sti &= ~S_UPDATE;

		return cmem[addr]; // The value of crm is ignored during an update.
	}
	else
	{
		int crm = (st1 & ST1_CRM) >> ST1_CRM_SHIFT;
		u32 cvar = cmem[addr];
		if (crm == 1)
			return (cvar & 0xffff0000);
		else if (crm == 2)
			return (cvar << 16);
		return cvar;
	}
}

void tms57002_device::cache_flush()
{
	int i;
	cache.hused = cache.iused = 0;
	for (i = 0; i != 256; i++)
		cache.hashbase[i] = -1;
	for (i = 0; i != HBS; i++)
	{
		cache.hashnode[i].st1 = 0;
		cache.hashnode[i].ipc = -1;
		cache.hashnode[i].next = -1;
	}
	for (i = 0; i != IBS; i++)
	{
		cache.inst[i].op = 0;
		cache.inst[i].next = -1;
		cache.inst[i].param = 0;
	}
}

void tms57002_device::add_one(cstate *cs, u16 op, u8 param)
{
	s16 ipc = cache.iused++;
	cache.inst[ipc].op = op;
	cache.inst[ipc].param = param;
	cache.inst[ipc].next = -1;
	if (cs->ipc != -1)
		cache.inst[cs->ipc].next = ipc;
	cs->ipc = ipc;
	if (cs->hnode != -1)
	{
		cache.hashnode[cs->hnode].ipc = ipc;
		cs->hnode = -1;
	}
}

void tms57002_device::decode_one(u32 opcode, cstate *cs, void (tms57002_device::*dec)(u32 opcode, u16 *op, cstate *cs))
{
	u16 op = 0;
	(this->*dec)(opcode, &op, cs);
	if (!op)
		return;
	add_one(cs, op, opcode & 0xff);
}

s16 tms57002_device::get_hash(u8 adr, u32 st1, s16 *pnode)
{
	s16 hnode;
	st1 &= ST1_CACHE;
	*pnode = -1;
	hnode = cache.hashbase[adr];
	while(hnode != -1)
	{
		if (cache.hashnode[hnode].st1 == st1)
			return cache.hashnode[hnode].ipc;
		*pnode = hnode;
		hnode = cache.hashnode[hnode].next;
	}
	return -1;
}

s16 tms57002_device::get_hashnode(u8 adr, u32 st1, s16 pnode)
{
	s16 hnode = cache.hused++;
	cache.hashnode[hnode].st1 = st1 & ST1_CACHE;
	cache.hashnode[hnode].ipc = -1;
	cache.hashnode[hnode].next = -1;
	if (pnode == -1)
		cache.hashbase[adr] = hnode;
	else
		cache.hashnode[pnode].next = hnode;
	return hnode;
}

int tms57002_device::decode_get_pc()
{
	s16 pnode, res;
	cstate cs;
	u8 adr = pc;

	res = get_hash(adr, st1, &pnode);
	if (res != -1)
		return res;

	if (HBS - cache.hused < 256 || IBS - cache.iused < 256*3)
	{
		cache_flush();
		pnode = -1;
	}

	cs.hnode = res = get_hashnode(adr, st1, pnode);
	cs.ipc = -1;
	cs.branch = 0;

	for (;;)
	{
		s16 ipc;
		u32 opcode = program_ram[adr];

		cs.inc = 0;

		if ((opcode & 0xfc0000) == 0xfc0000)
			decode_one(opcode, &cs, &tms57002_device::decode_cat3);
		else {
			decode_one(opcode, &cs, &tms57002_device::decode_cat2_pre);
			decode_one(opcode, &cs, &tms57002_device::decode_cat1);
			decode_one(opcode, &cs, &tms57002_device::decode_cat2_post);
		}
		add_one(&cs, cs.inc, 0);

		if (cs.branch)
			break;

		adr++;
		ipc = get_hash(adr, st1, &pnode);
		if (ipc != -1)
		{
			cache.inst[cs.ipc].next = ipc;
			break;
		}
		cs.hnode = get_hashnode(adr, st1, pnode);
	}

	return cache.hashnode[res].ipc;
}

void tms57002_device::execute_run()
{
	int ipc = -1;

	while(icount > 0 && !(sti & (S_IDLE | IN_PLOAD /*| IN_CLOAD*/)))
	{
		int iipc;

		/* debugger hook omitted: no host debugger is attached in this adapter. */

		if (ipc == -1)
			ipc = decode_get_pc();

		iipc = ipc;

		if (sti & (S_READ|S_WRITE))
		{
			if (sti & S_READ)
				xm_step_read();
			else
				xm_step_write();
		}

		macc_read = macc_write;
		macc_write = macc;

		for (;;)
		{
			const icd *i = cache.inst + ipc;

			ipc = i->next;
			switch (i->op)
			{
			case 0:
				goto inst;

			case 1:
				++ca;
				goto inst;

			case 2:
				++id;
				goto inst;

			case 3:
				++ca, ++id;
				goto inst;

#define CINTRPSWITCH
#include "tms57002.hxx"
#undef CINTRPSWITCH

			default:
				assert(false && "Unhandled TMS57002 micro-op");
			}
		}
	inst:
		icount--;

		if (rptc)
		{
			rptc--;
			ipc = iipc;
		}
		else if (sti & S_BRANCH)
		{
			sti &= ~S_BRANCH;
			ipc = -1;
		}
		else
		{
			pc++; // Wraps if it reaches 256, next wraps too
			update_pc0();
		}

		if (rptc_next)
		{
			rptc = rptc_next;
			rptc_next = 0;
		}
	}

	if (icount > 0)
		icount = 0;
}

void tms57002_device::process_sample(const float input[4], float output[4])
{
    const float in_scale = 32768.0f * ((st0 & ST0_SIM) ? 256.0f : 1.0f);
    for (unsigned i = 0; i < 4; ++i) {
        const s32 scaled = static_cast<s32>(input[i] * in_scale);
        si[i] = static_cast<u32>(scaled) & 0xffffff;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const s32 shifted = static_cast<s32>(so[i] << 8);
        output[i] = static_cast<float>(shifted / 2147483648.0);
    }
    sync_w(1);
}
