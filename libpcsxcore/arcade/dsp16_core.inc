// Generated from MAME: BSD-3-Clause, copyright Vas Crabb.
class dsp16_device_base::core_state
{
public:
	// construction/destruction
	constexpr core_state(u8 yaau_bits) : yaau_mask(u16((u32(1) << yaau_bits) - 1)), yaau_sign(u16(1) << (yaau_bits - 1)) { }
	~core_state() = default;

	// setup


	// execution state helpers
	constexpr bool icount_remaining() const { return 0 < icount; }
	void decrement_icount() { --icount; ++cycles; }

	// XAAU helpers
	constexpr u16 xaau_next_pc() const { return (xaau_pc & XAAU_I_EXT) | ((xaau_pc + 1) & XAAU_I_MASK); }
	void xaau_increment_pt(u16 op) { xaau_pt = (xaau_pt & XAAU_I_EXT) | ((xaau_pt + op_xaau_increment(op)) & XAAU_I_MASK); }
	void xaau_extend_i() { xaau_i = (xaau_i & XAAU_I_MASK) | ((xaau_i & XAAU_I_SIGN) ? XAAU_I_EXT : 0); }

	// YAAU helpers
	u16 &yaau_postmodify_r(u16 op);
	s16 &yaau_set_j(s16 value) { return yaau_j = yaau_extend(value); }
	s16 &yaau_set_k(s16 value) { return yaau_k = yaau_extend(value); }
	u16 &yaau_set_rb(s16 value) { return yaau_rb = value & yaau_mask; }
	u16 &yaau_set_re(s16 value) { return yaau_re = value & yaau_mask; }
	constexpr s16 yaau_extend(s16 value) const { return (value & yaau_mask) | ((value & yaau_sign) ? ~yaau_mask : 0); }
	s16 &yaau_extend_j() { return yaau_j = yaau_extend(yaau_j); }
	s16 &yaau_extend_k() { return yaau_k = yaau_extend(yaau_k); }

	// DAU helpers
	u64 dau_f1(u16 op);
	void dau_f2(u16 op);
	constexpr s16 dau_get_y(u16 op) const { return u16(u32(dau_y) >> (op_x(op) ? 16 : 0)); }
	void dau_set_y(u16 op, s16 value) { op_x(op) ? dau_set_y(value) : dau_set_yl(value); }
	void dau_set_y(s16 value) { dau_y = (u32(u16(value)) << 16) | u32(BIT(dau_auc, 6) ? u16(u32(dau_y)) : u16(0)); }
	void dau_set_yl(s16 value) { dau_y = (dau_y & ~((u32(1) << 16) - 1)) | u16(value); }
	s64 &dau_set_atx(u16 op, s16 value);
	s64 &dau_set_at(u16 op, s16 value);
	template <unsigned T> void dau_extend_a() { dau_a[T] = (dau_a[T] & DAU_A_MASK) | ((dau_a[T] & DAU_A_SIGN) ? DAU_A_EXT : 0); }
	u16 dau_export_psw()
	{
		return dau_psw = (dau_psw & 0xfe10U) | (u16(u64(dau_a[0]) >> 32) & 0x000fU) | (u16(u64(dau_a[1]) >> 27) & 0x01e0U);
	}
	void dau_import_psw()
	{
		dau_a[0] = u64(u32(dau_a[0])) | (u64(dau_psw & 0x000fU) << 32) | (BIT(dau_psw, 3) ? DAU_A_EXT : 0U);
		dau_a[1] = u64(u32(dau_a[1])) | (u64(dau_psw & 0x01e0U) << 27) | (BIT(dau_psw, 8) ? DAU_A_EXT : 0U);
	}

	// DAU flag accessors
	constexpr bool dau_auc_sat(u16 a) const { return bool(BIT(dau_auc, 2 + a)); }
	constexpr u16 dau_auc_align() const { return dau_auc & 0x0003U; }
	constexpr bool dau_psw_lmi() const { return bool(BIT(dau_psw, 15)); }
	constexpr bool dau_psw_leq() const { return bool(BIT(dau_psw, 14)); }
	constexpr bool dau_psw_llv() const { return bool(BIT(dau_psw, 13)); }
	constexpr bool dau_psw_lmv() const { return bool(BIT(dau_psw, 12)); }

	// opcode field handling
	constexpr s16 op_xaau_increment(u16 op) const { return op_x(op) ? xaau_i : 1; }
	u16 &op_yaau_r(u16 op) { return yaau_r[(op >> 2) & 0x0003U]; }
	s64 &op_dau_as(u16 op) { return dau_a[op_s(op)]; }
	s64 &op_dau_ad(u16 op) { return dau_a[op_d(op)]; }
	s64 &op_dau_at(u16 op) { return dau_a[op_d(~op)]; }

	// configuration
	u16 const   yaau_mask;
	u16 const   yaau_sign;

	// execution state
	int     icount = 0;
	uint64_t cycles = 0;

	// XAAU - ROM Address Arithmetic Unit
	u16         xaau_pc = 0U;                   // 16 bits unsigned
	u16         xaau_pt = 0U;                   // 16 bits unsigned
	u16         xaau_pr = 0U;                   // 16 bits unsigned
	u16         xaau_pi = 0U;                   // 16 bits unsigned
	s16         xaau_i = 0;                     // 12 bits signed

	// YAAU - RAM Address Arithmetic Unit
	u16         yaau_r[4] = { 0U, 0U, 0U, 0U }; // 9/16 bits unsigned
	u16         yaau_rb = 0U;                   // 9/16 bits unsigned
	u16         yaau_re = 0U;                   // 9/16 bits unsigned
	s16         yaau_j = 0;                     // 9/16 bits signed
	s16         yaau_k = 0;                     // 9/16 bits signed

	// DAU - Data Arithmetic Unit
	s16         dau_x = 0;                      // 16 bits signed
	s32         dau_y = 0;                      // 32 bits signed
	s32         dau_p = 0;                      // 32 bits signed
	s64         dau_a[2] = { 0, 0 };            // 36 bits signed
	s8          dau_c[3] = { 0, 0, 0 };         // 8 bits signed
	u8          dau_auc = 0U;                   // 7 bits unsigned
	u16         dau_psw = 0U;                   // 16 bits
	s16         dau_temp = 0;                   // 16 bits

private:
	// internal helpers
	u64 dau_get_p_aligned() const;
	u64 dau_set_psw_flags(s64 d);
};


inline u16 &dsp16_device_base::core_state::yaau_postmodify_r(u16 op)
{
	u16 &r(op_yaau_r(op));
	switch (op & 0x0003U)
	{
	case 0x0: // *rN
		break;
	case 0x1: // *rN++
		r = (yaau_re && (yaau_re == r)) ? yaau_rb : (r + 1);
		break;
	case 0x2: // *rN--
		--r;
		break;
	case 0x3: // *rN++j
		r += yaau_j;
		break;
	}
	return r &= yaau_mask;
}

/***********************************************************************
    DAU helpers
***********************************************************************/

inline u64 dsp16_device_base::core_state::dau_f1(u16 op)
{
	s64 const &s(op_dau_as(op));
	s64 d(0);
	switch (op_f1(op))
	{
	case 0x0: // aD = p ; p = x*y
		d = dau_get_p_aligned();
		dau_p = dau_x * (dau_y >> 16);
		break;
	case 0x1: // aD = aS + p ; p = x*y
		d = s + dau_get_p_aligned();
		dau_p = dau_x * (dau_y >> 16);
		break;
	case 0x2: // p = x*y
		dau_p = dau_x * (dau_y >> 16);
		return op_dau_ad(op);
	case 0x3: // aD = aS - p ; p = x*y
		d = s - dau_get_p_aligned();
		dau_p = dau_x * (dau_y >> 16);
		break;
	case 0x4: // aD = p
		d = dau_get_p_aligned();
		break;
	case 0x5: // aD = aS + p
		d = s + dau_get_p_aligned();
		break;
	case 0x6: // NOP
		return op_dau_ad(op);
	case 0x7: // aD = aS - p
		d = s - dau_get_p_aligned();
		break;
	case 0x8: // aD = aS | y
		d = s | dau_y;
		break;
	case 0x9: // aD = aS ^ y
		d = s ^ dau_y;
		break;
	case 0xa: // aS & y
		dau_set_psw_flags(s & dau_y);
		return op_dau_ad(op);
	case 0xb: // aS - y
		dau_set_psw_flags(s - dau_y);
		return op_dau_ad(op);
	case 0xc: // aD = y
		d = dau_y;
		break;
	case 0xd: // aD = aS + y
		d = s + dau_y;
		break;
	case 0xe: // aD = aS & y
		d = s & dau_y;
		break;
	case 0xf: // aD = aS - y
		d = s - dau_y;
		break;
	}
	return dau_set_psw_flags(d);
}

inline void dsp16_device_base::core_state::dau_f2(u16 op)
{
	s64 const &s(op_dau_as(op));
	s64 &d(op_dau_ad(op));
	switch (op_f2(op))
	{
	case 0x0: // aD = aS >> 1
		d = s >> 1;
		break;
	case 0x1: // aD = aS << 1
		d = s32(u32(u64(s)) << 1);
		break;
	case 0x2: // aD = aS >> 4
		d = s >> 4;
		break;
	case 0x3: // aD = aS << 4
		d = s32(u32(u64(s)) << 4);
		break;
	case 0x4: // aD = aS >> 8
		d = s >> 8;
		break;
	case 0x5: // aD = aS << 8
		d = s32(u32(u64(s)) << 8);
		break;
	case 0x6: // aD = aS >> 16
		d = s >> 16;
		break;
	case 0x7: // aD = aS << 16
		d = s32(u32(u64(s)) << 16);
		break;
	case 0x8: // aD = p
		d = dau_get_p_aligned();
		break;
	case 0x9: // aDh = aSh + 1
		d = s64(s32(u32(u64(s)) & ~((u32(1) << 16) - 1))) + (s32(1) << 16);
		if (BIT(dau_auc, op_s(op) + 4))
			d |= op_dau_ad(op) & ((s64(1) << 16) - 1);
		break;
	case 0xa: // Reserved
		throw emu_fatalerror("DSP16: reserved F2 value %01X (PC = %04X)\n", op_f2(op), xaau_pc/*FIXME: st_pcbase*/);
	case 0xb: // aD = rnd(aS)
		// FIXME: behaviour is not clear
		// p 3-13: "Round upper 20 bits of accumulator."
		// p 3-14: "The contents of the source accumulator, aS, are rounded to 16 bits, and the sign-extended result is placed in aD[35 - 16] with zeroes in aD[15 - 0]."
		// It presumably rounds to nearest, but does it yield a 16-bit or 20-bit result, and what does it do about overflow?
		d = (s + ((s32(1) << 15) - ((0 > s) ? 1 : 0))) & ~((u64(1) << 16) - 1);
		break;
	case 0xc: // aD = y
		d = dau_y;
		break;
	case 0xd: // aD = aS + 1
		d = s + 1;
		break;
	case 0xe: // aD = aS
		d = s;
		break;
	case 0xf: // aD = -aS
		// FIXME: does this detect negation of largest negative number as overflow?
		d = -s;
		break;
	}
	d = dau_set_psw_flags(d);
}

inline s64 &dsp16_device_base::core_state::dau_set_atx(u16 op, s16 value)
{
	s64 &at(op_dau_at(op));
	if (op_x(op))
	{
		bool const clear(!BIT(dau_psw, 4 + op_d(~op)));
		return at = s32((u32(u16(value)) << 16) | u32(clear ? u16(0) : u16(u64(at))));
	}
	else
	{
		return at = (at & ~((s64(1) << 16) - 1)) | u64(u16(value));
	}
}

inline s64 &dsp16_device_base::core_state::dau_set_at(u16 op, s16 value)
{
	s64 &at(op_dau_at(op));
	bool const clear(!BIT(dau_psw, 4 + op_d(~op)));
	return at = s32((u32(u16(value)) << 16) | u32(clear ? u16(0) : u16(u64(at))));
}

/***********************************************************************
    internal helpers
***********************************************************************/

inline u64 dsp16_device_base::core_state::dau_get_p_aligned() const
{
	// TODO: manual is contradictory
	// p 2-10: "Bits 1 and 2 of the accumulator are not changed by the load of the accumulator with the data in p, since 00 is added to or copied into these accumulator bits as indicated in Figure 2-8."
	// I'm reading this as copying p to aD clears the low two bits, but adding it leaves them unchanged.
	switch (dau_auc_align())
	{
	case 0x0:
		return dau_p;
	case 0x1:
		return dau_p >> 2;
	case 0x2:
		return u64(dau_p) << 2;
	case 0x3:
	default:
		throw emu_fatalerror("DSP16: reserved ALIGN value %01X (PC = %04X)\n", dau_auc_align(), xaau_pc/*FIXME: st_pcbase*/);
	}
}

inline u64 dsp16_device_base::core_state::dau_set_psw_flags(s64 d)
{
	dau_psw &= 0x0fffU;
	bool const negative(d & DAU_A_SIGN);
	if (negative)
		dau_psw |= 0x8000U;
	if (!(d & DAU_A_MASK))
		dau_psw |= 0x4000U;
	if ((d >> 36) != (negative ? -1 : 0))
		dau_psw |= 0x2000U;
	if (((d >> 32) & ((1 << 4) - 1)) != (BIT(d, 31) ? 15 : 0))
		dau_psw |= 0x1000U;
	if (negative)
		return d | DAU_A_EXT;
	else
		return d & DAU_A_MASK;
}

