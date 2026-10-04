// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// Adapted from MAME h8.cpp.
u8 H8CPU::do_addx8(u8 v1, u8 v2)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	u16 res = v1 + v2 + c;
	m_CCR &= ~(F_N|F_V|F_C);
	if(m_has_hc) {
		if(((v1 & 0xf) + (v2 & 0xf) + c) & 0x10)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(u8(res))
		m_CCR &= ~F_Z;
	if(s8(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80)
		m_CCR |= F_V;
	if(res & 0x100)
		m_CCR |= F_C;
	return res;
}

u8 H8CPU::do_subx8(u8 v1, u8 v2)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	u16 res = v1 - v2 - c;
	m_CCR &= ~(F_N|F_V|F_C);
	if(m_has_hc) {
		if(((v1 & 0xf) - (v2 & 0xf) - c) & 0x10)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(u8(res))
		m_CCR &= ~F_Z;
	if(s8(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x80)
		m_CCR |= F_V;
	if(res & 0x100)
		m_CCR |= F_C;
	return res;
}

u8 H8CPU::do_inc8(u8 v1, u8 v2)
{
	u8 res = v1 + v2;
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!res)
		m_CCR |= F_Z;
	else if(s8(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80)
		m_CCR |= F_V;
	return res;
}

u16 H8CPU::do_inc16(u16 v1, u16 v2)
{
	u16 res = v1 + v2;
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!res)
		m_CCR |= F_Z;
	else if(s16(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x8000)
		m_CCR |= F_V;
	return res;
}

u32 H8CPU::do_inc32(u32 v1, u32 v2)
{
	u32 res = v1 + v2;
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!res)
		m_CCR |= F_Z;
	else if(s32(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80000000)
		m_CCR |= F_V;
	return res;
}

u8 H8CPU::do_add8(u8 v1, u8 v2)
{
	u16 res = v1 + v2;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(m_has_hc) {
		if(((v1 & 0xf) + (v2 & 0xf)) & 0x10)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(!u8(res))
		m_CCR |= F_Z;
	else if(s8(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80)
		m_CCR |= F_V;
	if(res & 0x100)
		m_CCR |= F_C;
	return res;
}

u16 H8CPU::do_add16(u16 v1, u16 v2)
{
	u32 res = v1 + v2;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(m_has_hc) {
		if(((v1 & 0xfff) + (v2 & 0xfff)) & 0x1000)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(!u16(res))
		m_CCR |= F_Z;
	else if(s16(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x8000)
		m_CCR |= F_V;
	if(res & 0x10000)
		m_CCR |= F_C;
	return res;
}

u32 H8CPU::do_add32(u32 v1, u32 v2)
{
	u64 res = u64(v1) + u64(v2);
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(m_has_hc) {
		if(((v1 & 0xfffffff) + (v2 & 0xfffffff)) & 0x10000000)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(!u32(res))
		m_CCR |= F_Z;
	else if(s32(res) < 0)
		m_CCR |= F_N;
	if(~(v1^v2) & (v1^res) & 0x80000000)
		m_CCR |= F_V;
	if(res & 0x100000000ULL)
		m_CCR |= F_C;
	return res;
}

u8 H8CPU::do_dec8(u8 v1, u8 v2)
{
	u8 res = v1 - v2;
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!res)
		m_CCR |= F_Z;
	else if(s8(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x80)
		m_CCR |= F_V;
	return res;
}

u16 H8CPU::do_dec16(u16 v1, u16 v2)
{
	u16 res = v1 - v2;
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!res)
		m_CCR |= F_Z;
	else if(s16(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x8000)
		m_CCR |= F_V;
	return res;
}

u32 H8CPU::do_dec32(u32 v1, u32 v2)
{
	u32 res = v1 - v2;
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!res)
		m_CCR |= F_Z;
	else if(s32(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x80000000)
		m_CCR |= F_V;
	return res;
}

u8 H8CPU::do_sub8(u8 v1, u8 v2)
{
	u16 res = v1 - v2;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(m_has_hc) {
		if(((v1 & 0xf) - (v2 & 0xf)) & 0x10)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(!u8(res))
		m_CCR |= F_Z;
	else if(s8(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x80)
		m_CCR |= F_V;
	if(res & 0x100)
		m_CCR |= F_C;
	return res;
}

u16 H8CPU::do_sub16(u16 v1, u16 v2)
{
	u32 res = v1 - v2;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(m_has_hc) {
		if(((v1 & 0xfff) - (v2 & 0xfff)) & 0x1000)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(!u16(res))
		m_CCR |= F_Z;
	else if(s16(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x8000)
		m_CCR |= F_V;
	if(res & 0x10000)
		m_CCR |= F_C;
	return res;
}

u32 H8CPU::do_sub32(u32 v1, u32 v2)
{
	u64 res = u64(v1) - u64(v2);
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(m_has_hc) {
		if(((v1 & 0xfffffff) - (v2 & 0xfffffff)) & 0x10000000)
			m_CCR |= F_H;
		else
			m_CCR &= ~F_H;
	}
	if(!u32(res))
		m_CCR |= F_Z;
	else if(s32(res) < 0)
		m_CCR |= F_N;
	if((v1^v2) & (v1^res) & 0x80000000)
		m_CCR |= F_V;
	if(res & 0x100000000ULL)
		m_CCR |= F_C;
	return res;
}

u8 H8CPU::do_shal8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		m_CCR |= F_C;
	if((v & 0xc0) == 0x40 || (v & 0xc0) == 0x80)
		m_CCR |= F_V;
	v <<= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_shal16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		m_CCR |= F_C;
	if((v & 0xc000) == 0x4000 || (v & 0xc000) == 0x8000)
		m_CCR |= F_V;
	v <<= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_shal32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		m_CCR |= F_C;
	if((v & 0xc0000000) == 0x40000000 || (v & 0xc0000000) == 0x80000000)
		m_CCR |= F_V;
	v <<= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_shar8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		m_CCR |= F_C;
	v >>= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(v & 0x40) {
		v |= 0x80;
		m_CCR |= F_N;
	}
	return v;
}

u16 H8CPU::do_shar16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		m_CCR |= F_C;
	v >>= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(v & 0x4000) {
		v |= 0x8000;
		m_CCR |= F_N;
	}
	return v;
}

u32 H8CPU::do_shar32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		m_CCR |= F_C;
	v >>= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(v & 0x40000000) {
		v |= 0x80000000;
		m_CCR |= F_N;
	}
	return v;
}

u8 H8CPU::do_shll8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		m_CCR |= F_C;
	v <<= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_shll16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		m_CCR |= F_C;
	v <<= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_shll32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		m_CCR |= F_C;
	v <<= 1;
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_shlr8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		m_CCR |= F_C;
	v >>= 1;
	if(!v)
		m_CCR |= F_Z;
	return v;
}

u16 H8CPU::do_shlr16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		m_CCR |= F_C;
	v >>= 1;
	if(!v)
		m_CCR |= F_Z;
	return v;
}

u32 H8CPU::do_shlr32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 1)
		m_CCR |= F_C;
	v >>= 1;
	if(!v)
		m_CCR |= F_Z;
	return v;
}

u8 H8CPU::do_shal2_8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40)
		m_CCR |= F_C;
	if((v & 0xc0) == 0x40 || (v & 0xc0) == 0x80 ||
		(v & 0x60) == 0x20 || (v & 0x60) == 0x40)
		m_CCR |= F_V;
	v <<= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_shal2_16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x4000)
		m_CCR |= F_C;
	if((v & 0xc000) == 0x4000 || (v & 0xc000) == 0x8000 ||
		(v & 0x6000) == 0x2000 || (v & 0x6000) == 0x4000)
		m_CCR |= F_V;
	v <<= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_shal2_32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40000000)
		m_CCR |= F_C;
	if((v & 0xc0000000) == 0x40000000 || (v & 0xc0000000) == 0x80000000 ||
		(v & 0x60000000) == 0x20000000 || (v & 0x60000000) == 0x40000000)
		m_CCR |= F_V;
	v <<= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_shar2_8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 2)
		m_CCR |= F_C;
	v >>= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(v & 0x20) {
		v |= 0xc0;
		m_CCR |= F_N;
	}
	return v;
}

u16 H8CPU::do_shar2_16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 2)
		m_CCR |= F_C;
	v >>= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(v & 0x2000) {
		v |= 0xc000;
		m_CCR |= F_N;
	}
	return v;
}

u32 H8CPU::do_shar2_32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 2)
		m_CCR |= F_C;
	v >>= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(v & 0x20000000) {
		v |= 0xc0000000;
		m_CCR |= F_N;
	}
	return v;
}

u8 H8CPU::do_shll2_8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40)
		m_CCR |= F_C;
	v <<= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_shll2_16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x4000)
		m_CCR |= F_C;
	v <<= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_shll2_32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40000000)
		m_CCR |= F_C;
	v <<= 2;
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_shlr2_8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 2)
		m_CCR |= F_C;
	v >>= 2;
	if(!v)
		m_CCR |= F_Z;
	return v;
}

u16 H8CPU::do_shlr2_16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 2)
		m_CCR |= F_C;
	v >>= 2;
	if(!v)
		m_CCR |= F_Z;
	return v;
}

u32 H8CPU::do_shlr2_32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 2)
		m_CCR |= F_C;
	v >>= 2;
	if(!v)
		m_CCR |= F_Z;
	return v;
}

u8 H8CPU::do_rotl8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		m_CCR |= F_C;
	v = (v << 1) | (v >> 7);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotl16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		m_CCR |= F_C;
	v = (v << 1) | (v >> 15);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotl32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		m_CCR |= F_C;
	v = (v << 1) | (v >> 31);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotr8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x01)
		m_CCR |= F_C;
	v = (v << 7) | (v >> 1);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotr16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x0001)
		m_CCR |= F_C;
	v = (v << 15) | (v >> 1);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotr32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x00000001)
		m_CCR |= F_C;
	v = (v << 31) | (v >> 1);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotxl8(u8 v)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80)
		m_CCR |= F_C;
	v = (v << 1) | c;
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotxl16(u16 v)
{
	u16 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x8000)
		m_CCR |= F_C;
	v = (v << 1) | c;
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotxl32(u32 v)
{
	u32 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x80000000)
		m_CCR |= F_C;
	v = (v << 1) | c;
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotxr8(u8 v)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x01)
		m_CCR |= F_C;
	v = (v >> 1) | (c << 7);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotxr16(u16 v)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x0001)
		m_CCR |= F_C;
	v = (v >> 1) | (c << 15);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotxr32(u32 v)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x00000001)
		m_CCR |= F_C;
	v = (v >> 1) | (c << 31);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotl2_8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40)
		m_CCR |= F_C;
	v = (v << 2) | (v >> 6);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotl2_16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x4000)
		m_CCR |= F_C;
	v = (v << 2) | (v >> 14);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotl2_32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40000000)
		m_CCR |= F_C;
	v = (v << 2) | (v >> 30);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotr2_8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x02)
		m_CCR |= F_C;
	v = (v << 6) | (v >> 2);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotr2_16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x0002)
		m_CCR |= F_C;
	v = (v << 14) | (v >> 2);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotr2_32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x00000002)
		m_CCR |= F_C;
	v = (v << 30) | (v >> 2);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotxl2_8(u8 v)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40)
		m_CCR |= F_C;
	v = (v << 2) | (c << 1) | (v >> 7);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotxl2_16(u16 v)
{
	u16 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x4000)
		m_CCR |= F_C;
	v = (v << 2) | (c << 1) | (v >> 15);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotxl2_32(u32 v)
{
	u32 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x40000000)
		m_CCR |= F_C;
	v = (v << 2) | (c << 1) | (v >> 31);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

u8 H8CPU::do_rotxr2_8(u8 v)
{
	u8 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x02)
		m_CCR |= F_C;
	v = (v >> 2) | (c << 6) | (v << 7);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
	return v;
}

u16 H8CPU::do_rotxr2_16(u16 v)
{
	u16 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x0002)
		m_CCR |= F_C;
	v = (v >> 2) | (c << 14) | (v << 15);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
	return v;
}

u32 H8CPU::do_rotxr2_32(u32 v)
{
	u32 c = m_CCR & F_C ? 1 : 0;
	m_CCR &= ~(F_N|F_V|F_Z|F_C);
	if(v & 0x00000002)
		m_CCR |= F_C;
	v = (v >> 2) | (c << 30) | (v << 31);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
	return v;
}

void H8CPU::set_nzv8(u8 v)
{
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!v)
		m_CCR |= F_Z;
	else if(s8(v) < 0)
		m_CCR |= F_N;
}

void H8CPU::set_nzv16(u16 v)
{
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
}

void H8CPU::set_nzv32(u32 v)
{
	m_CCR &= ~(F_N|F_V|F_Z);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
}

void H8CPU::set_nz16(u16 v)
{
	m_CCR &= ~(F_N|F_Z);
	if(!v)
		m_CCR |= F_Z;
	else if(s16(v) < 0)
		m_CCR |= F_N;
}

void H8CPU::set_nz32(u32 v)
{
	m_CCR &= ~(F_N|F_Z);
	if(!v)
		m_CCR |= F_Z;
	else if(s32(v) < 0)
		m_CCR |= F_N;
}

