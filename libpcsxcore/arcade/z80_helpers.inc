// Generated from MAME; license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller, Andrei I. Holub
u8 z80_device::get_f()
{
	u8 f = 0;
	f |= m_f.s();
	f |= m_f.z();
	f |= m_f.yx();
	f |= m_f.h();
	f |= m_f.pv();
	f |= m_f.n ? NF : 0;
	f |= m_f.c ? CF : 0;
	return f;
}

void z80_device::set_f(u8 f)
{
	m_f.s_val  = f;
	m_f.z_val  = !(f & ZF);
	m_f.yx_val = f;
	m_f.h_val  = f;
	m_f.pv_val = !(f & PF);
	m_f.n      = f & NF;
	m_f.c      = f & CF;
}


/***************************************************************
 * Enter halt state; write 1 to callback on first execution
 ***************************************************************/
void z80_device::halt()
{
	if (!m_halt)
	{
		m_halt = 1;
		set_service_attention<SA_HALT, 1>();
		m_halt_cb(1);
	}
}

/***************************************************************
 * Leave halt state; write 0 to callback
 ***************************************************************/
void z80_device::leave_halt()
{
	if (m_halt)
	{
		m_halt = 0;
		set_service_attention<SA_HALT, 0>();
		m_halt_cb(0);
	}
}

/***************************************************************
 * Read a byte from given memory location
 ***************************************************************/
u8 z80_device::data_read(u16 addr)
{
	return memory_read(addr);
}

/***************************************************************
 * Write a byte to given memory location
 ***************************************************************/
void z80_device::data_write(u16 addr, u8 value)
{
	memory_write(addr, value);
}

/***************************************************************
 * rop() is identical to rm() except it is used for
 * reading opcodes. In case of system with memory mapped I/O,
 * this function can be used to greatly speed up emulation
 ***************************************************************/
u8 z80_device::opcode_read()
{
	return memory_opcode(PC);
}

/****************************************************************
 * arg() is identical to rop() except it is used
 * for reading opcode arguments. This difference can be used to
 * support systems that use different encoding mechanisms for
 * opcodes and opcode arguments
 * out: TDAT8
 ***************************************************************/
u8 z80_device::arg_read()
{
	return memory_read(PC);
}

/***************************************************************
 * INC  r8
 ***************************************************************/
void z80_device::inc(u8 &r)
{
	++r;
	{
		QT = 0;
		// keep C
		m_f.s_val = m_f.z_val = m_f.yx_val = r;
		m_f.pv_val = r != 0x80;
		m_f.h_val = (r & 0x0f) == 0x00 ? HF : 0;
		m_f.n = 0;
	}
}

/***************************************************************
 * DEC  r8
 ***************************************************************/
void z80_device::dec(u8 &r)
{
	--r;
	{
		QT = 0;
		// keep C
		m_f.s_val = m_f.z_val = m_f.yx_val = r;
		m_f.pv_val = r != 0x7f;
		m_f.h_val = (r & 0x0f) == 0x0f ? HF : 0;
		m_f.n = 1;
	}
}

/***************************************************************
 * RLCA
 ***************************************************************/
void z80_device::rlca()
{
	A = (A << 1) | (A >> 7);
	{
		QT = 0;
		// keep SZP
		m_f.yx_val = A;
		m_f.h_val = m_f.n = 0;
		m_f.c = A & 0x01;
	}
}

/***************************************************************
 * RRCA
 ***************************************************************/
void z80_device::rrca()
{
	const u8 a0 = A;
	A = (a0 >> 1) | (a0 << 7);
	{
		QT = 0;
		// keep SZP
		m_f.yx_val = A;
		m_f.h_val = m_f.n = 0;
		m_f.c = a0 & 0x01;
	}
}

/***************************************************************
 * RLA
 ***************************************************************/
void z80_device::rla()
{
	u8 res = (A << 1) + m_f.c;
	{
		QT = 0;
		// keep SZP
		m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = A & 0x80;
	}
	A = res;
}

/***************************************************************
 * RRA
 ***************************************************************/
void z80_device::rra()
{
	u8 res = (m_f.c << 7) | (A >> 1);
	{
		QT = 0;
		// keep SZP
		m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = A & 0x01;
	}
	A = res;
}

/***************************************************************
 * ADD  A,n
 ***************************************************************/
void z80_device::add_a(u8 value)
{
	const u16 res = A + value;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.yx_val = res;
		m_f.c = res & 0x100;
		m_f.h_val = (A & 0x0f) + (value & 0x0f);
		m_f.pv_val = !((A ^ res) & (value ^ res) & 0x80);
		m_f.n = 0;
	}
	A = res;
}

/***************************************************************
 * ADC  A,n
 ***************************************************************/
void z80_device::adc_a(u8 value)
{
	const int c = m_f.c;
	const u16 res = A + value + c;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.yx_val = res;
		m_f.c = res & 0x100;
		m_f.h_val = (A & 0x0f) + (value & 0x0f) + c;
		m_f.pv_val = !((A ^ res) & (value ^ res) & 0x80);
		m_f.n = 0;
	}
	A = res;
}

/***************************************************************
 * SUB  A,n
 ***************************************************************/
void z80_device::sub_a(u8 value)
{
	const u16 res = A - value;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.yx_val = res;
		m_f.c = res & 0x100;
		m_f.h_val = (A & 0x0f) - (value & 0x0f);
		m_f.pv_val = !((A ^ value) & (A ^ res) & 0x80);
		m_f.n = 1;
	}
	A = res;
}

/***************************************************************
 * SBC  A,n
 ***************************************************************/
void z80_device::sbc_a(u8 value)
{
	const int c = m_f.c;
	const u16 res = A - value - c;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.yx_val = res;
		m_f.c = res & 0x100;
		m_f.h_val = (A & 0x0f) - (value & 0x0f) - c;
		m_f.pv_val = !((A ^ value) & (A ^ res) & 0x80);
		m_f.n = 1;
	}
	A = res;
}

/***************************************************************
 * NEG
 ***************************************************************/
void z80_device::neg()
{
	u8 value = A;
	A = 0;
	sub_a(value);
}

/***************************************************************
 * DAA
 ***************************************************************/
void z80_device::daa()
{
	u8 a = A;
	if (m_f.n)
	{
		if (m_f.h() || ((A & 0xf) > 9)) a -= 6;
		if (m_f.c || (A > 0x99)) a -= 0x60;
	}
	else
	{
		if (m_f.h() || ((A & 0xf) > 9)) a += 6;
		if (m_f.c || (A > 0x99)) a += 0x60;
	}
	{
		QT = 0;
		// keep N
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = a;
		m_f.h_val = A ^ a;
		m_f.c = m_f.c || A > 0x99;
	}
	A = a;
}

/***************************************************************
 * AND  n
 ***************************************************************/
void z80_device::and_a(u8 value)
{
	A &= value;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = A;
		m_f.n = m_f.c = 0;
		m_f.h_val = HF;
	}
}

/***************************************************************
 * OR   n
 ***************************************************************/
void z80_device::or_a(u8 value)
{
	A |= value;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = A;
		m_f.h_val = m_f.n = m_f.c = 0;
	}
}

/***************************************************************
 * XOR  n
 ***************************************************************/
void z80_device::xor_a(u8 value)
{
	A ^= value;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = A;
		m_f.h_val = m_f.n = m_f.c = 0;
	}
}

/***************************************************************
 * CP   n
 ***************************************************************/
void z80_device::cp(u8 value)
{
	const u16 res = A - value;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = res;
		m_f.yx_val = value;
		m_f.c = res & 0x100;
		m_f.h_val = (A & 0x0f) - (value & 0x0f);
		m_f.pv_val = !((A ^ value) & (A ^ res) & 0x80);
		m_f.n = 1;
	}
}

/***************************************************************
 * EXX
 ***************************************************************/
void z80_device::exx()
{
	using std::swap;
	swap(m_bc, m_bc2);
	swap(m_de, m_de2);
	swap(m_hl, m_hl2);
}

/***************************************************************
 * RLC  r8
 ***************************************************************/
u8 z80_device::rlc(u8 value)
{
	const u8 res = ((value << 1) | (value >> 7)) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x80;
	}

	return res;
}

/***************************************************************
 * RRC  r8
 ***************************************************************/
u8 z80_device::rrc(u8 value)
{
	const u8 res = ((value >> 1) | (value << 7)) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x01;
	}

	return res;
}

/***************************************************************
 * RL   r8
 ***************************************************************/
u8 z80_device::rl(u8 value)
{
	const u8 res = ((value << 1) + m_f.c) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x80;
	}

	return res;
}

/***************************************************************
 * RR   r8
 ***************************************************************/
u8 z80_device::rr(u8 value)
{
	const u8 res = ((value >> 1) | (m_f.c << 7)) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x01;
	}

	return res;
}

/***************************************************************
 * SLA  r8
 ***************************************************************/
u8 z80_device::sla(u8 value)
{
	const u8 res = (value << 1) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x80;
	}

	return res;
}

/***************************************************************
 * SRA  r8
 ***************************************************************/
u8 z80_device::sra(u8 value)
{
	const u8 res = ((value >> 1) | (value & 0x80)) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x01;
	}

	return res;
}

/***************************************************************
 * SLL  r8
 ***************************************************************/
u8 z80_device::sll(u8 value)
{
	const u8 res = ((value << 1) | 0x01) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x80;
	}

	return res;
}

/***************************************************************
 * SRL  r8
 ***************************************************************/
u8 z80_device::srl(u8 value)
{
	const u8 res = (value >> 1) & 0xff;
	{
		QT = 0;
		m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = res;
		m_f.h_val = m_f.n = 0;
		m_f.c = value & 0x01;
	}

	return res;
}

/***************************************************************
 * BIT  bit,r8
 ***************************************************************/
void z80_device::bit(int bit, u8 value)
{
	QT = 0;
	m_f.s_val = m_f.z_val = m_f.pv_val = value & (1 << bit);
	m_f.h_val = HF;
	m_f.n = 0;
	m_f.yx_val = value;
}

/***************************************************************
 * BIT  bit,(HL)
 ***************************************************************/
void z80_device::bit_hl(int bit, u8 value)
{
	QT = 0;
	m_f.s_val = m_f.z_val = m_f.pv_val = value & (1 << bit);
	m_f.h_val = HF;
	m_f.n = 0;
	m_f.yx_val = WZ_H;
}

/***************************************************************
 * BIT  bit,(IX/Y+o)
 ***************************************************************/
void z80_device::bit_xy(int bit, u8 value)
{
	QT = 0;
	m_f.s_val = m_f.z_val = m_f.pv_val = value & (1 << bit);
	m_f.h_val = HF;
	m_f.n = 0;
	m_f.yx_val = m_ea >> 8;
}

/***************************************************************
 * RES  bit,r8
 ***************************************************************/
u8 z80_device::res(int bit, u8 value)
{
	return value & ~(1 << bit);
}

/***************************************************************
 * SET  bit,r8
 ***************************************************************/
u8 z80_device::set(int bit, u8 value)
{
	return value | (1 << bit);
}

void z80_device::block_io_interrupted_flags()
{
	m_f.yx_val = PC >> 8;

	const u8 pv_old = m_f.pv();
	if (m_f.c)
	{
		m_f.h_val = 0;
		if (TDAT8 & 0x80)
		{
			m_f.pv_val = (B - 1) & 0x07;
			if ((B & 0x0f) == 0x00) m_f.h_val = HF;
		}
		else
		{
			m_f.pv_val = (B + 1) & 0x07;
			if ((B & 0x0f) == 0x0f) m_f.h_val = HF;
		}
	}
	else
	{
		m_f.pv_val = B & 0x07;
	}
	m_f.pv_val = (pv_old ^ m_f.pv()) & PF;
}

/***************************************************************
 * EI
 ***************************************************************/
void z80_device::ei()
{
	m_iff1 = m_iff2 = 1;
	set_service_attention<SA_AFTER_EI, 1>();
}

void z80_device::illegal_1() { undocumented_count++; }

void z80_device::illegal_2() { undocumented_count++; }
