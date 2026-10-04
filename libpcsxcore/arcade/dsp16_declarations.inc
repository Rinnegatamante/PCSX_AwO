// Generated from MAME: BSD-3-Clause, copyright Vas Crabb.
	// masks for registers that aren't power-of-two sizes
	enum : s16
	{
		XAAU_I_SIGN = s16(1) << (12 - 1),
		XAAU_I_MASK = (s16(1) << 12) - 1,
		XAAU_I_EXT = ~XAAU_I_MASK
	};
	enum : s64
	{
		DAU_A_SIGN = s64(1) << (36 - 1),
		DAU_A_MASK = (s64(1) << 36) - 1,
		DAU_A_EXT = ~DAU_A_MASK
	};

	// execution state
	enum class cache : u8 { NONE, LOAD, EXECUTE };
	enum class phase : u8 { PURGE, OP1, OP2, PREFETCH };
	enum flags : u8
	{
		FLAGS_PRED_NONE     = 0x00U,
		FLAGS_PRED_TRUE     = 0x01U,
		FLAGS_PRED_FALSE    = 0x02U,
		FLAGS_PRED_MASK     = 0x03U,

		FLAGS_IACK_NONE     = 0x00U,
		FLAGS_IACK_SET      = 0x04U,
		FLAGS_IACK_CLEAR    = 0x08U,
		FLAGS_IACK_MASK     = 0x0cU,

		FLAGS_NONE          = FLAGS_PRED_NONE | FLAGS_IACK_NONE
	};
	friend constexpr flags operator~(flags);
	friend constexpr flags operator&(flags, flags);
	friend constexpr flags operator|(flags, flags);
	friend flags &operator&=(flags &, flags);
	friend flags &operator|=(flags &, flags);

	// serial I/O state
	enum sio_flags : u8
	{
		SIO_FLAGS_NONE      = 0x00U,
		SIO_FLAGS_ILD       = 0x01U,
		SIO_FLAGS_OLD       = 0x02U
	};
	friend constexpr sio_flags operator~(sio_flags);
	friend constexpr sio_flags operator&(sio_flags, sio_flags);
	friend constexpr sio_flags operator|(sio_flags, sio_flags);
	friend sio_flags &operator&=(sio_flags &, sio_flags);
	friend sio_flags &operator|=(sio_flags &, sio_flags);

	// instruction execution
	template <bool Debugger, bool Caching> void execute_some_rom();
	template <bool Debugger> void execute_some_cache();
	void overlap_rom_data_read();
	void yaau_short_immediate_load(u16 op);
	template <bool Debugger> s16 yaau_read(u16 op);
	template <bool Debugger> void yaau_write(u16 op, s16 value);
	template <bool Debugger> void yaau_write_z(u16 op);

	// built-in peripherals
	void sio_step();
	void pio_step();

	// inline helpers
	static bool op_interruptible(u16 op);
	bool check_predicate();
	flags &set_predicate(flags predicate) { return m_flags = (m_flags & ~FLAGS_PRED_MASK) | (predicate & FLAGS_PRED_MASK); }
	flags &set_iack(flags iack) { return m_flags = (m_flags & ~FLAGS_IACK_MASK) | (iack & FLAGS_IACK_MASK); }
	u16 &set_xaau_pc_offset(u16 offset);
	s16 get_r(u16 op);
	void set_r(u16 op, s16 value);
	s64 dau_saturate(u16 a) const;
	bool op_dau_con(u16 op, bool inc);

	// opcode field handling
	static constexpr u16 op_ja(u16 op) { return op & 0x0fffU; }
	static constexpr u16 op_b(u16 op) { return (op >> 8) & 0x0007U; }
	static constexpr u16 op_d(u16 op) { return BIT(op, 10); }
	static constexpr u16 op_s(u16 op) { return BIT(op, 9); }
	static constexpr u16 op_f1(u16 op) { return (op >> 5) & 0x000fU; }
	static constexpr u16 op_f2(u16 op) { return (op >> 5) & 0x000fU; }
	static constexpr u16 op_r(u16 op) { return (op >> 4) & 0x003fU; }
	static constexpr u16 op_x(u16 op) { return BIT(op, 4); }
	static constexpr u16 op_con(u16 op) { return op & 0x001fU; }
	static constexpr u16 op_ni(u16 op) { return (op >> 7) & 0x000fU; }
	static constexpr u16 op_k(u16 op) { return op & 0x007fU; }

	// serial I/O
	bool sio_ld_ick() const { return !BIT(m_sio_sioc, 9); }
	bool sio_ld_ock() const { return bool(BIT(m_sio_sioc, 9)); }
	bool sio_lsb_first() const { return !BIT(m_sio_sioc, 6); }
	bool sio_msb_first() const { return bool(BIT(m_sio_sioc, 6)); }
	bool sio_old_active() const { return bool(BIT(m_sio_sioc, 5)); }
	bool sio_ild_active() const { return bool(BIT(m_sio_sioc, 4)); }
	bool sio_ock_active() const { return bool(BIT(m_sio_sioc, 3)); }
	bool sio_ick_active() const { return bool(BIT(m_sio_sioc, 2)); }
	unsigned sio_olen() const { return BIT(m_sio_sioc, 1) ? 8U : 16U; }
	unsigned sio_ilen() const { return BIT(m_sio_sioc, 0) ? 8U : 16U; }
	void sio_sioc_write(u16 value);
	void sio_ick_active_edge();
	void sio_ock_active_edge();
	void sio_step_ld_div();

	// parallel I/O
	u16 pio_strobe() const { return ((m_pio_pioc >> 13) & 0x0003U) + 1; }
	bool pio_pods_active() const { return bool(BIT(m_pio_pioc, 12)); }
	bool pio_pids_active() const { return bool(BIT(m_pio_pioc, 11)); }
	bool pio_sc_mode() const { return bool(BIT(m_pio_pioc, 10)); }
	bool pio_ibf_enable() const { return bool(BIT(m_pio_pioc, 9)); }
	bool pio_obe_enable() const { return bool(BIT(m_pio_pioc, 8)); }
	bool pio_pids_enable() const { return bool(BIT(m_pio_pioc, 7)); }
	bool pio_pods_enable() const { return bool(BIT(m_pio_pioc, 6)); }
	bool pio_int_enable() const { return bool(BIT(m_pio_pioc, 5)); }
	bool pio_ibf_status() const { return bool(BIT(m_pio_pioc, 4)); }
	bool pio_obe_status() const { return bool(BIT(m_pio_pioc, 3)); }
	bool pio_pids_status() const { return bool(BIT(m_pio_pioc, 2)); }
	bool pio_pods_status() const { return bool(BIT(m_pio_pioc, 1)); }
	bool pio_int_status() const { return bool(BIT(m_pio_pioc, 0)); }
	void pio_pioc_write(u16 value);
	u16 pio_pdx_read(u16 sel);
	void pio_pdx_write(u16 sel, u16 value);

	// execution state
	cache       m_cache_mode;
	phase       m_phase;
	u8          m_int_enable[2];
	flags       m_flags;
	u8          m_cache_ptr, m_cache_limit, m_cache_iterations;
	u16         m_cache[16];
	u16         m_rom_data;

	// line states
	u8          m_exm_in;       // internal ROM disabled when low
	u8          m_int_in;       // external interrupt request
	u8          m_iack_out;     // asserted (low) while servicing interrupt

	// serial I/O line states
	u8          m_ick_in;       // data input clock - sampled on rising edge
	u8          m_ild_in;       // data input clock - sampled on rising edge
	u8          m_do_out;       // serial data output - changes on rising edges of OCK
	u8          m_ock_in;       // data output clock - output changes on rising edge
	u8          m_old_in;       // falling edge indicates beginning of output word
	u8          m_ose_out;      // indicates the end of a serial transmission

	// parallel I/O line states
	u8          m_psel_out;     // last accessed parallel I/O data register
	u8          m_pids_out;     // parallel input data strobe (sampled on rising edge)
	u8          m_pods_out;     // parallel output data strobe

	// SIO - Serial I/O
	u16         m_sio_sioc;     // 10 bits
	u16         m_sio_obuf;     // 16 bits
	u16         m_sio_osr;      // 16 bits
	u16         m_sio_ofsr;     // 16 bits
	u8          m_sio_clk;
	u8          m_sio_clk_div;
	u8          m_sio_clk_res;
	u8          m_sio_ld;
	u8          m_sio_ld_div;
	sio_flags   m_sio_flags;

	// PIO - Parallel I/O
	u16         m_pio_pioc;     // 16 bits
	u16         m_pio_pdx_in;   // 16 bits
	u16         m_pio_pdx_out;  // 16 bits
	u8          m_pio_pids_cnt;
	u8          m_pio_pods_cnt;

	// fake registers for the debugger
	u16         m_cache_pcbase;
	u16         m_st_pcbase;
	u16         m_st_genflags;
	s16         m_st_yh, m_st_ah[2];
	u16         m_st_yl, m_st_al[2];