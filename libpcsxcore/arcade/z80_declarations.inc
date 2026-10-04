// Generated from MAME; license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller, Andrei I. Holub
	void illegal_1();
	void illegal_2();
	template <u8 Bit, bool State> void set_service_attention() { static_assert(Bit < 8, "out of range bit index"); if (State) m_service_attention |= (1 << Bit); else m_service_attention &= ~(1 << Bit); };
	template <u8 Bit> bool get_service_attention() { static_assert(Bit < 8, "out of range bit index"); return m_service_attention & (1 << Bit); };

	void halt();
	void leave_halt();
	void inc(u8 &r);
	void dec(u8 &r);
	void rlca();
	void rrca();
	void rla();
	void rra();
	void add_a(u8 value);
	void adc_a(u8 value);
	void sub_a(u8 value);
	void sbc_a(u8 value);
	void neg();
	void daa();
	void and_a(u8 value);
	void or_a(u8 value);
	void xor_a(u8 value);
	void cp(u8 value);
	void exx();
	u8 rlc(u8 value);
	u8 rrc(u8 value);
	u8 rl(u8 value);
	u8 rr(u8 value);
	u8 sla(u8 value);
	u8 sra(u8 value);
	u8 sll(u8 value);
	u8 srl(u8 value);
	void bit(int bit, u8 value);
	void bit_hl(int bit, u8 value);
	void bit_xy(int bit, u8 value);
	u8 res(int bit, u8 value);
	u8 set(int bit, u8 value);
	void ei();
	void block_io_interrupted_flags();

	u8 data_read(u16 addr);
	void data_write(u16 addr, u8 value);
	u8 stack_read(u16 addr) { return data_read(addr); }
	void stack_write(u16 addr, u8 value) { return data_write(addr, value); }
	u8 opcode_read();
	u8 arg_read();

	static constexpr u8 SA_BUSREQ        = 0;
	static constexpr u8 SA_NMI_PENDING   = 1;
	static constexpr u8 SA_IRQ_ON        = 2;
	static constexpr u8 SA_HALT          = 3;
	static constexpr u8 SA_AFTER_EI      = 4;
	static constexpr u8 SA_AFTER_LDAIR   = 5;
	static constexpr u8 SA_NSC800_IRQ_ON = 6;
	u8 m_service_attention; // bitmap for required handling in service step

	PAIR16 m_prvpc;
	PAIR16 m_pc;
	PAIR16 m_sp;
	PAIR16 m_af;
	PAIR16 m_bc;
	PAIR16 m_de;
	PAIR16 m_hl;
	PAIR16 m_ix;
	PAIR16 m_iy;
	PAIR16 m_wz;
	PAIR16 m_af2;
	PAIR16 m_bc2;
	PAIR16 m_de2;
	PAIR16 m_hl2;
	u8     m_r;
	u8     m_r2;
	bool   m_iff1;
	bool   m_iff2;
	u8     m_halt;
	u8     m_im;
	u8     m_i;
	u8     m_nmi_state;    // nmi pin state
	u8     m_irq_state;    // irq pin state
	int    m_wait_state;   // wait pin state
	int    m_busreq_state; // bus request pin state
	u8     m_busack_state; // bus acknowledge pin state
	u16    m_ea;

	struct
	{
		u8 s_val;  // bit 7, other bits = don't care
		u8 z_val;  // 0 or not 0
		u8 yx_val; // bit 5 for Y, bit 3 for X, other bits = don't care
		u8 h_val;  // bit 4, other bits = don't care
		u8 pv_val; // overflow case set in the way that pv() returns desired value
		bool n;
		bool c;

		u8 q;      // CCF/SCF YX mask
		u8 qtemp;

		u8 s() const { return s_val & 0x80; }
		u8 z() const { return z_val ? 0 : 0x40; }
		u8 yx() const { return yx_val & 0x28; }
		u8 h() const { return h_val & 0x10; }
		u8 pv() const {
			u8 val = pv_val;
			val ^= val >> 4;
			val ^= val << 2;
			val ^= val >> 1;
			return ~val & 0x04;
		}
	} m_f;
	u8 get_f();
	void set_f(u8 f);

	int    m_icount;
	int    m_tmp_irq_vector;
	PAIR16 m_shared_data;
	PAIR16 m_shared_data2;
	u8     m_rtemp;

	u32    m_ref;
	u8     m_m1_cycles;
	u8     m_mreq_cycles;
	u8     m_iorq_cycles;