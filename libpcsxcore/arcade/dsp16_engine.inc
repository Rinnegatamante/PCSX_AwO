// Generated from MAME: BSD-3-Clause, copyright Vas Crabb.
u16 dsp16_device_base::pio_r()
{
	if (!pio_pods_active())
	{
		LOGPIO("DSP16: PIO host read PSEL = %u, PDX = %04X (PC = %04X)\n", m_psel_out, m_pio_pdx_out, m_st_pcbase);
		if (!pio_pods_status())
			LOGINT("DSP16: set PODS flag (PC = %04X)\n", m_st_pcbase);
		m_pio_pioc |= u16(1) << 1;
		return m_pio_pdx_out;
	}
	else
	{
		logerror("DSP16: warning: PIO host read in active mode - returning line status (PC = %04X)\n", m_st_pcbase);
		// TODO: improve this when low-level interface is more complete
		return !m_pods_out ? m_pio_pdx_out : 0xffffU;
	}
}

void dsp16_device_base::pio_w(u16 data)
{
	if (!pio_pids_active())
	{
		LOGPIO("DSP16: PIO host write PSEL = %u, PDX = %04X (PC = %04X)\n", m_psel_out, data, m_st_pcbase);
		if (!pio_pids_status())
			LOGINT("DSP16: set PIDS flag (PC = %04X)\n", m_st_pcbase);
		m_pio_pioc |= u16(1) << 2;
		m_pio_pdx_in = data;
	}
	else
	{
		logerror("DSP16: warning: ignoring PIO host write in active mode (PC = %04X)\n", m_st_pcbase);
	}
}

void dsp16_device_base::device_reset()
{


	m_cache_mode = cache::NONE;
	m_phase = phase::PURGE;
	std::fill(std::begin(m_int_enable), std::end(m_int_enable), 0U);
	m_flags = FLAGS_NONE;
	m_cache_ptr = 0U;
	m_core->xaau_pc = 0U;
	m_core->yaau_rb = 0U;
	m_core->yaau_re = 0U;
	m_sio_sioc = 0U;
	m_sio_ofsr = 0U;
	m_sio_clk = 1U;
	m_sio_clk_div = 0U;
	m_sio_clk_res = 0U;
	m_pio_pioc = 0x0008U;
	m_pio_pids_cnt = 0U;
	m_pio_pods_cnt = 0U;
	m_st_pcbase = 0U;

	// interrupt reset outputs
	if (!m_iack_out)
	{
		LOGINT("DSP16: de-asserting IACK for reset\n");
		m_iack_cb(m_iack_out = 1U);
	}

	// SIO reset outputs
	m_ick_cb(1); // actually high-impedance
	m_ild_cb(1); // actually high-impedance
	m_do_cb(m_do_out = 1U); // actually high-impedance
	m_ock_cb(1); // actually high-impedance
	m_old_cb(1); // actually high-impedance
	if (!m_ose_out)
		m_ose_cb(m_ose_out = 1U);

	// PIO reset outputs
	m_pdb_w_cb(m_psel_out, 0xffffU, 0x0000U);
	if (!m_pids_out)
	{
		LOGPIO("DSP16: de-asserting PIDS for reset\n");
		m_pids_cb(m_pids_out = 1U); // actually high-impedance
	}
	if (!m_pods_out)
	{
		LOGPIO("DSP16: de-asserting PODS for reset\n");
		m_pods_cb(m_pods_out = 1U); // actually high-impedance
	}
}

void dsp16_device_base::execute_set_input(int inputnum, int state)
{
	switch (inputnum)
	{
	case DSP16_INT_LINE:
		m_int_in = state;
		break;
	case DSP16_ICK_LINE:
		if (((CLEAR_LINE == state) ? 0U : 1U) != m_ick_in)
		{
			if (!sio_ick_active())
			{
				if (CLEAR_LINE != state)
					sio_ick_active_edge();
			}
			else
			{
				logerror("DSP16: warning: ignoring ICK input in active mode (PC = %04X)\n", m_st_pcbase);
			}
			m_ick_in = (ASSERT_LINE == state) ? 1U : 0U;
		}
		if (CLEAR_LINE != state)
			standard_irq_callback(DSP16_ICK_LINE, m_st_pcbase);
		break;
	case DSP16_ILD_LINE:
		if (sio_ild_active())
			logerror("DSP16: warning: ignoring ILD input in active mode (PC = %04X)\n", m_st_pcbase);
		m_ild_in = state;
		break;
	case DSP16_OCK_LINE:
		if (((CLEAR_LINE == state) ? 0U : 1U) != m_ock_in)
		{
			if (!sio_ock_active())
			{
				if (CLEAR_LINE != state)
					sio_ock_active_edge();
			}
			else
			{
				logerror("DSP16: warning: ignoring OCK input in active mode (PC = %04X)\n", m_st_pcbase);
			}
			m_ock_in = (ASSERT_LINE == state) ? 1U : 0U;
		}
		if (CLEAR_LINE != state)
			standard_irq_callback(DSP16_OCK_LINE, m_st_pcbase);
		break;
	case DSP16_OLD_LINE:
		if (sio_old_active())
			logerror("DSP16: warning: ignoring OLD input in active mode (PC = %04X)\n", m_st_pcbase);
		m_old_in = state;
		break;
	}
}

template <bool Debugger, bool Caching> inline void dsp16_device_base::execute_some_rom()
{
	DSP16_REQUIRE(debugger_enabled() == Debugger);
	for (bool mode_change = false; !mode_change && m_core->icount_remaining(); m_core->decrement_icount())
	{
		DSP16_REQUIRE((cache::LOAD == m_cache_mode) == Caching);

		u16 const op(m_cache[m_cache_ptr]);
		bool const last_cache_load(Caching && (m_cache_ptr == m_cache_limit));
		u16 const cache_next((m_cache_ptr + (Caching ? 1 : 0)) & 0x0fU);
		u16 *fetch_target(&m_cache[cache_next]);
		u16 fetch_addr(0U);
		flags predicate(FLAGS_PRED_NONE);

		switch (m_phase)
		{
		case phase::PURGE:
			fetch_addr = m_core->xaau_pc;
			m_phase = phase::OP1;
			break;

		case phase::OP1:
			++instructions;
			if (Debugger)
			{
				if (FLAGS_PRED_NONE == (m_flags & FLAGS_PRED_MASK))
				{
					debugger_instruction_hook(m_st_pcbase);
				}
				else
				{
					switch (op >> 11)
					{
					case 0x00: // goto JA
					case 0x01:
					case 0x10: // call JA
					case 0x11:
						break;
					case 0x18: // goto B
						switch (op_b(op))
						{
						case 0x0: // return
						case 0x2: // goto pt
						case 0x3: // call pt
							break;
						default:
							debugger_instruction_hook(m_st_pcbase);
						}
						break;
					default:
						debugger_instruction_hook(m_st_pcbase);
					}
				}
			}

			// IACK is updated for the next instruction
			switch (m_flags & FLAGS_IACK_MASK)
			{
			case FLAGS_IACK_SET:
				if (m_iack_out)
				{
					LOGINT("DSP16: asserting IACK (PC = %04X)\n", m_st_pcbase);
					m_iack_cb(m_iack_out = 0U);
					standard_irq_callback(DSP16_INT_LINE, m_st_pcbase);
				}
				break;
			case FLAGS_IACK_CLEAR:
				if (!m_iack_out)
				{
					LOGINT("DSP16: de-asserting IACK (PC = %04X)\n", m_st_pcbase);
					m_iack_cb(m_iack_out = 1U);
					m_pio_pioc &= 0xfffeU;
				}
				break;
			default:
				break;
			}
			set_iack(FLAGS_IACK_NONE);

			// if we're not servicing an interrupt or caching
			if (m_iack_out && !Caching)
			{
				// TODO: is INT sampled on any instruction or only interruptible instructions?
				// if an unmasked interrupt is pending
				if ((m_pio_pioc & m_int_enable[0] & 0x001eU) || (BIT(m_int_enable[0], 0) && (CLEAR_LINE != m_int_in)))
				{
					// if the current instruction is interruptible
					if (op_interruptible(op))
					{
						if (pio_int_enable() && (CLEAR_LINE != m_int_in))
						{
							if (ASSERT_LINE != m_int_in)
								m_int_in = CLEAR_LINE;
							m_pio_pioc |= 0x0001U;
						}
						LOGINT(
								"DSP16: servicing interrupts%s%s%s%s%s (PC = %04X)\n",
								(pio_ibf_enable() && pio_ibf_status()) ? " IBF" : "",
								(pio_obe_enable() && pio_obe_status()) ? " OBE" : "",
								(pio_pids_enable() && pio_pids_status()) ? " PIDS" : "",
								(pio_pods_enable() && pio_pods_status()) ? " PODS" : "",
								(pio_int_enable() && pio_int_status()) ? " INT" : "",
								m_st_pcbase);
						set_iack(FLAGS_IACK_SET);
						fetch_addr = m_core->xaau_next_pc();
						m_int_enable[0] = m_int_enable[1];
						m_core->xaau_pc = 0x0001U;
						m_phase = phase::PURGE;
						break;
					}
				}
			}

			// normal opcode execution
			fetch_addr = set_xaau_pc_offset(m_core->xaau_pc + 1);
			m_int_enable[0] = m_int_enable[1];
			switch (op >> 11)
			{
			case 0x00: // goto JA
			case 0x01:
			case 0x10: // call JA
			case 0x11:
				if (check_predicate())
				{
					if (BIT(op, 15))
						m_core->xaau_pr = m_core->xaau_pc;
					set_xaau_pc_offset(op_ja(op));
				}
				m_phase = phase::PURGE;
				break;

			case 0x02: // R = M
			case 0x03:
				yaau_short_immediate_load(op);
				break;

			case 0x04: // F1 ; Y = a1[l]
			case 0x1c: // F1 ; Y = a0[l]
				fetch_target = nullptr;
				m_phase = phase::OP2;
				break;

			case 0x05: // F1 ; Z : aT[l]
				{
					fetch_target = nullptr;
					s64 const d(m_core->dau_f1(op));
					m_core->dau_temp = u16(u64(dau_saturate(op_d(~op))) >> (op_x(op) ? 16 : 0));
					m_core->op_dau_ad(op) = d;
					m_core->dau_set_atx(op, yaau_read<Debugger>(op));
					m_phase = phase::OP2;
				}
				break;

			case 0x06: // F1 ; Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				yaau_read<Debugger>(op);
				break;

			case 0x07: // F1 ; aT[l] = Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_set_atx(op, yaau_read<Debugger>(op));
				break;

			case 0x08: // aT = R
				DSP16_REQUIRE(!(op & 0x000fU)); // reserved field?
				fetch_target = nullptr;
				m_core->dau_set_at(op, get_r(op));
				m_phase = phase::OP2;
				break;

			case 0x09: // R = a0
			case 0x0b: // R = a1
				DSP16_REQUIRE(!(op & 0x040fU)); // reserved fields?
				fetch_target = nullptr;
				set_r(op, u16(u64(dau_saturate(BIT(op, 12))) >> 16));
				m_phase = phase::OP2;
				break;

			case 0x0a: // R = N
				DSP16_REQUIRE(!(op & 0x040fU)); // reserved fields?
				m_phase = phase::OP2;
				fetch_target = &m_rom_data;
				break;

			case 0x0c: // Y = R
				DSP16_REQUIRE(!(op & 0x0400U)); // reserved field?
				fetch_target = nullptr;
				m_phase = phase::OP2;
				break;

			case 0x0d: // Z : R
				fetch_target = nullptr;
				m_core->dau_temp = get_r(op);
				set_r(op, yaau_read<Debugger>(op));
				m_phase = phase::OP2;
				break;

			case 0x0e: // do K { instr1...instrNI } # redo K
				{
					u16 const ni(op_ni(op));
					if (ni)
					{
						mode_change = true;
						fetch_target = &m_cache[m_cache_ptr = 1U];
						m_cache_mode = cache::LOAD;
						m_cache_limit = ni;
						m_cache_pcbase = m_st_pcbase;
					}
					else
					{
						mode_change = true;
						fetch_target = nullptr;
						m_cache_mode = cache::EXECUTE;
						m_phase = phase::PREFETCH;
						m_cache_ptr = 1U;
					}
					m_cache_iterations = op_k(op);
					DSP16_REQUIRE(m_cache_iterations >= 2U); // p3-25: "The iteration count can be between 2 and 127, inclusive"
				}
				break;

			case 0x0f: // R = Y
				DSP16_REQUIRE(!(op & 0x0400U)); // reserved field?
				fetch_target = nullptr;
				set_r(op, yaau_read<Debugger>(op));
				m_phase = phase::OP2;
				break;

			case 0x12: // ifc CON F2
				{
					bool const con(op_dau_con(op, false));
					++m_core->dau_c[1];
					if (con)
					{
						m_core->dau_f2(op);
						m_core->dau_c[2] = m_core->dau_c[1];
					}
				}
				break;

			case 0x13: // if CON F2
				if (op_dau_con(op, true))
					m_core->dau_f2(op);
				break;

			case 0x14: // F1 ; Y = y[l]
				fetch_target = nullptr;
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_phase = phase::OP2;
				break;

			case 0x15: // F1 ; Z : y[l]
				fetch_target = nullptr;
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_temp = m_core->dau_get_y(op);
				m_core->dau_set_y(op, yaau_read<Debugger>(op));
				m_phase = phase::OP2;
				break;

			case 0x16: // F1 ; x = Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_x = yaau_read<Debugger>(op);
				break;

			case 0x17: // F1 ; y[l] = Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_set_y(op, yaau_read<Debugger>(op));
				break;

			case 0x18: // goto B
				DSP16_REQUIRE(!(op & 0x00ffU)); // reserved field?
				switch (op_b(op))
				{
				case 0x0: // return
					if (check_predicate())
						m_core->xaau_pc = m_core->xaau_pr;
					break;
				case 0x1: // ireturn
					if (m_iack_out)
						logerror("DSP16: ireturn when not servicing interrupt (PC = %04X)\n", m_st_pcbase);
					LOGINT("DSP16: return from interrupt (PC = %04X)\n", m_st_pcbase);
					set_iack(FLAGS_IACK_CLEAR);
					m_core->xaau_pc = m_core->xaau_pi;
					break;
				case 0x2: // goto pt
					if (check_predicate())
						m_core->xaau_pc = m_core->xaau_pt;
					break;
				case 0x3: // call pt
					if (check_predicate())
					{
						m_core->xaau_pr = m_core->xaau_pc;
						m_core->xaau_pc = m_core->xaau_pt;
					}
					break;
				case 0x4: // Reserved
				case 0x5:
				case 0x6:
				case 0x7:
					throw emu_fatalerror("DSP16: reserved B value %01X (PC = %04X)\n", op_b(op), m_st_pcbase);
				}
				if (m_iack_out)
					m_core->xaau_pi = m_core->xaau_pc;
				m_phase = phase::PURGE;
				break;

			case 0x19: // F1 ; y = a0 ; x = *pt++[i]
			case 0x1b: // F1 ; y = a1 ; x = *pt++[i]
				{
					DSP16_REQUIRE(!(op & 0x000fU)); // reserved field?
					s64 const d(m_core->dau_f1(op));
					s64 a(m_core->dau_a[BIT(op, 12)]);
					// FIXME: is saturation applied when transferring a to y?
					m_core->dau_y = u32(u64(a));
					m_core->op_dau_ad(op) = d;
					fetch_target = nullptr;
					m_rom_data = program_read(m_core->xaau_pt);
					m_phase = phase::OP2;
				}
				break;

			case 0x1a: // if CON # icall
				DSP16_REQUIRE(!(op & 0x03e0U)); // reserved field?
				predicate = op_dau_con(op, true) ? FLAGS_PRED_TRUE : FLAGS_PRED_FALSE;
				if (BIT(op, 10))
				{
					fetch_target = nullptr;
					DSP16_REQUIRE(0x000eU == op_con(op)); // CON must be true for icall?
					m_phase = phase::OP2;
				}
				break;

			case 0x1d: // F1 ; Z : y ; x = *pt++[i]
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_temp = s16(m_core->dau_y >> 16);
				m_core->dau_set_y(yaau_read<Debugger>(op));
				fetch_target = nullptr;
				m_rom_data = program_read(m_core->xaau_pt);
				m_phase = phase::OP2;
				break;

			case 0x1e: // Reserved
				throw emu_fatalerror("DSP16: reserved op %u (PC = %04X)\n", op >> 11, m_st_pcbase);

			case 0x1f: // F1 ; y = Y ; x = *pt++[i]
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_set_y(yaau_read<Debugger>(op));
				fetch_target = nullptr;
				m_rom_data = program_read(m_core->xaau_pt);
				m_phase = phase::OP2;
				break;

			default:
				throw emu_fatalerror("DSP16: unimplemented op %u (PC = %04X)\n", op >> 11, m_st_pcbase);
			}

			if (Caching && (phase::OP1 == m_phase))
			{
				// if the last instruction loaded to the cache completes in a single cycle, there's an extra cycle for a data fetch
				if (last_cache_load)
				{
					mode_change = true;
					fetch_target = nullptr;
					m_cache_mode = cache::EXECUTE;
					m_phase = phase::PREFETCH;
					m_cache_ptr = 1U;
					--m_cache_iterations;
				}
				else
				{
					m_cache_ptr = cache_next;
				}
			}
			break;

		case phase::OP2:
			m_phase = phase::OP1;
			switch (op >> 11)
			{
			case 0x04: // F1 ; Y = a1[l]
			case 0x1c: // F1 ; Y = a0[l]
				yaau_write<Debugger>(op, u16(u64(dau_saturate(BIT(~op, 14))) >> (op_x(op) ? 16 : 0)));
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				break;

			case 0x05: // F1 ; Z : aT[l]
			case 0x0d: // Z : R
			case 0x15: // F1 ; Z : y[l]
				yaau_write_z<Debugger>(op);
				break;

			case 0x08: // aT = R
			case 0x09: // R = a0
			case 0x0b: // R = a1
				break;

			case 0x0a: // R = N
				set_xaau_pc_offset(m_core->xaau_pc + 1);
				set_r(op, m_rom_data);
				break;

			case 0x0c: // Y = R
				yaau_write<Debugger>(op, get_r(op));
				break;

			case 0x0f: // R = Y
				break;

			case 0x14: // F1 ; Y = y[l]
				yaau_write<Debugger>(op, m_core->dau_get_y(op));
				break;

			case 0x19: // F1 ; y = a0 ; x = *pt++[i]
			case 0x1b: // F1 ; y = a1 ; x = *pt++[i]
			case 0x1f: // F1 ; y = Y ; x = *pt++[i]
				m_core->xaau_increment_pt(op);
				m_core->dau_x = m_rom_data;
				break;

			case 0x1a: // icall
				// TODO: does INT get sampled or could an external interrupt be lost here?
				DSP16_REQUIRE(BIT(op, 10));
				DSP16_REQUIRE(FLAGS_PRED_TRUE == (m_flags & FLAGS_PRED_MASK));
				if (check_predicate())
				{
					LOGINT(
							"DSP16: servicing software interrupt%s%s%s%s%s (PC = %04X)\n",
							(pio_ibf_enable() && pio_ibf_status()) ? " IBF" : "",
							(pio_obe_enable() && pio_obe_status()) ? " OBE" : "",
							(pio_pids_enable() && pio_pids_status()) ? " PIDS" : "",
							(pio_pods_enable() && pio_pods_status()) ? " PODS" : "",
							(pio_int_enable() && pio_int_status()) ? " INT" : "",
							m_st_pcbase);
					set_iack(FLAGS_IACK_SET);
					m_core->xaau_pc = 0x0002U;
				}
				m_phase = phase::PURGE;
				break;

			case 0x1d: // F1 ; Z : y ; x = *pt++[i]
				m_core->xaau_increment_pt(op);
				m_core->dau_x = m_rom_data;
				yaau_write_z<Debugger>(op);
				break;

			default:
				throw emu_fatalerror("DSP16: op %u doesn't take two cycles to run from ROM\n", op >> 11);
			}

			if (last_cache_load)
			{
				mode_change = true;
				fetch_target = nullptr;
				m_cache_mode = cache::EXECUTE;
				m_cache_ptr = 1U;
				--m_cache_iterations;
				overlap_rom_data_read();
			}
			else
			{
				fetch_addr = m_core->xaau_pc;
				if (Caching)
					m_cache_ptr = cache_next;
			}
			break;

		default:
			throw emu_fatalerror("DSP16: inappropriate phase for ROM execution\n");
		}

		if (FLAGS_PRED_NONE != (m_flags & FLAGS_PRED_MASK))
			throw emu_fatalerror("DSP16: predicate applied to ineligible op %u (PC = %04X)\n", op >> 11, m_st_pcbase);
		set_predicate(predicate);

		if (fetch_target)
			*fetch_target = program_read(fetch_addr);

		if (phase::OP1 == m_phase)
		{
			if (cache::EXECUTE != m_cache_mode)
				m_st_pcbase = m_core->xaau_pc;
			else
				m_st_pcbase = (m_cache_pcbase & 0xf000U) | ((m_cache_pcbase + m_cache_ptr) & 0x0fffU);
		}

		sio_step();
		pio_step();
	}
}

template <bool Debugger> inline void dsp16_device_base::execute_some_cache()
{
	DSP16_REQUIRE(debugger_enabled() == Debugger);
	for (bool mode_change = false; !mode_change && m_core->icount_remaining(); m_core->decrement_icount())
	{
		u16 const op(m_cache[m_cache_ptr]);
		bool const at_limit(m_cache_ptr == m_cache_limit);
		bool const last_instruction(at_limit && (1U == m_cache_iterations));
		switch (m_phase)
		{
		case phase::OP1:
			++instructions;
			if (Debugger)
				debugger_instruction_hook(m_st_pcbase);
			m_int_enable[0] = m_int_enable[1];
			switch (op >> 11)
			{
			case 0x02: // R = M
			case 0x03:
				yaau_short_immediate_load(op);
				break;

			case 0x04: // F1 ; Y = a1[l]
			case 0x1c: // F1 ; Y = a0[l]
				m_phase = phase::OP2;
				break;

			case 0x05: // F1 ; Z : aT[l]
				{
					s64 const d(m_core->dau_f1(op));
					m_core->dau_temp = u16(u64(dau_saturate(op_d(~op))) >> (op_x(op) ? 16 : 0));
					m_core->op_dau_ad(op) = d;
					m_core->dau_set_atx(op, yaau_read<Debugger>(op));
					m_phase = phase::OP2;
				}
				break;

			case 0x06: // F1 ; Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				yaau_read<Debugger>(op);
				break;

			case 0x07: // F1 ; aT[l] = Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_set_atx(op, yaau_read<Debugger>(op));
				break;

			case 0x08: // aT = R
				DSP16_REQUIRE(!(op & 0x000fU)); // reserved field?
				m_core->dau_set_at(op, get_r(op));
				m_phase = phase::OP2;
				break;

			case 0x09: // R = a0
			case 0x0b: // R = a1
				DSP16_REQUIRE(!(op & 0x040fU)); // reserved fields?
				set_r(op, u16(u64(dau_saturate(BIT(op, 12))) >> 16));
				m_phase = phase::OP2;
				break;

			case 0x0c: // Y = R
				DSP16_REQUIRE(!(op & 0x0400U)); // reserved field?
				m_phase = phase::OP2;
				break;

			case 0x0d: // Z : R
				m_core->dau_temp = get_r(op);
				set_r(op, yaau_read<Debugger>(op));
				m_phase = phase::OP2;
				break;

			case 0x0f: // R = Y
				DSP16_REQUIRE(!(op & 0x0400U)); // reserved field?
				set_r(op, yaau_read<Debugger>(op));
				m_phase = phase::OP2;
				break;

			case 0x12: // ifc CON F2
				{
					bool const con(op_dau_con(op, false));
					++m_core->dau_c[1];
					if (con)
					{
						m_core->dau_f2(op);
						m_core->dau_c[2] = m_core->dau_c[1];
					}
				}
				break;

			case 0x13: // if CON F2
				if (op_dau_con(op, true))
					m_core->dau_f2(op);
				break;

			case 0x14: // F1 ; Y = y[l]
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_phase = phase::OP2;
				break;

			case 0x15: // F1 ; Z : y[l]
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_temp = m_core->dau_get_y(op);
				m_core->dau_set_y(op, yaau_read<Debugger>(op));
				m_phase = phase::OP2;
				break;

			case 0x16: // F1 ; x = Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_x = yaau_read<Debugger>(op);
				break;

			case 0x17: // F1 ; y[l] = Y
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_set_y(op, yaau_read<Debugger>(op));
				break;

			case 0x19: // F1 ; y = a0 ; x = *pt++[i]
			case 0x1b: // F1 ; y = a1 ; x = *pt++[i]
				{
					DSP16_REQUIRE(!(op & 0x000fU)); // reserved field?
					s64 const d(m_core->dau_f1(op));
					s64 a(m_core->dau_a[BIT(op, 12)]);
					// FIXME: is saturation applied when transferring a to y?
					m_core->dau_y = u32(u64(a));
					m_core->op_dau_ad(op) = d;
					if (last_instruction)
					{
						m_rom_data = program_read(m_core->xaau_pt);
						m_phase = phase::OP2;
					}
					else
					{
						m_core->xaau_increment_pt(op);
						m_core->dau_x = m_rom_data;
					}
				}
				break;

			case 0x1d: // F1 ; Z : y ; x = *pt++[i]
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_temp = s16(m_core->dau_y >> 16);
				m_core->dau_set_y(yaau_read<Debugger>(op));
				m_rom_data = program_read(m_core->xaau_pt);
				m_phase = phase::OP2;
				break;

			case 0x1f: // F1 ; y = Y ; x = *pt++[i]
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				m_core->dau_set_y(yaau_read<Debugger>(op));
				if (last_instruction)
				{
					m_rom_data = program_read(m_core->xaau_pt);
					m_phase = phase::OP2;
				}
				else
				{
					m_core->xaau_increment_pt(op);
					m_core->dau_x = m_rom_data;
				}
				break;

			default:
				throw emu_fatalerror("DSP16: inelligible op %u in cache (PC = %04X)\n", op >> 11, m_st_pcbase);
			}
			break;

		case phase::OP2:
			m_phase = phase::OP1;
			switch (op >> 11)
			{
			case 0x04: // F1 ; Y = a1[l]
			case 0x1c: // F1 ; Y = a0[l]
				yaau_write<Debugger>(op, u16(u64(dau_saturate(BIT(~op, 14))) >> (op_x(op) ? 16 : 0)));
				m_core->op_dau_ad(op) = m_core->dau_f1(op);
				break;

			case 0x05: // F1 ; Z : aT[l]
			case 0x0d: // Z : R
			case 0x15: // F1 ; Z : y[l]
				yaau_write_z<Debugger>(op);
				break;

			case 0x08: // aT = R
			case 0x09: // R = a0
			case 0x0b: // R = a1
				break;

			case 0x0c: // Y = R
				yaau_write<Debugger>(op, get_r(op));
				break;

			case 0x0f: // R = Y
				break;

			case 0x14: // F1 ; Y = y[l]
				yaau_write<Debugger>(op, m_core->dau_get_y(op));
				break;

			case 0x19: // F1 ; y = a0 ; x = *pt++[i]
			case 0x1b: // F1 ; y = a1 ; x = *pt++[i]
			case 0x1f: // F1 ; y = Y ; x = *pt++[i]
				DSP16_REQUIRE(last_instruction);
				m_core->xaau_increment_pt(op);
				m_core->dau_x = m_rom_data;
				break;

			case 0x1d: // F1 ; Z : y ; x = *pt++[i]
				m_core->xaau_increment_pt(op);
				m_core->dau_x = m_rom_data;
				yaau_write_z<Debugger>(op);
				break;

			default:
				throw emu_fatalerror("DSP16: op %u doesn't take two cycles to run from cache\n", op >> 11);
			}
			break;

		case phase::PREFETCH:
			break;

		default:
			throw emu_fatalerror("DSP16: inappropriate phase for cache execution\n");
		}

		if (phase::PREFETCH == m_phase)
		{
			m_phase = phase::OP1;
			overlap_rom_data_read();
			m_st_pcbase = (m_cache_pcbase & 0xf000U) | ((m_cache_pcbase + m_cache_ptr) & 0x0fffU);
		}
		else if (phase::OP1 == m_phase)
		{
			if (last_instruction)
			{
				// overlapped fetch of next instruction from ROM
				mode_change = true;
				m_cache_mode = cache::NONE;
				m_cache[m_cache_ptr = 0] = program_read(m_core->xaau_pc);
				m_st_pcbase = m_core->xaau_pc;
			}
			else
			{
				if (at_limit)
				{
					// loop to first cached instruction
					m_cache_ptr = 1U;
					--m_cache_iterations;
				}
				else
				{
					// move to next cached instruction
					m_cache_ptr = (m_cache_ptr + 1) & 0x0fU;
				}
				overlap_rom_data_read();
				m_st_pcbase = (m_cache_pcbase & 0xf000U) | ((m_cache_pcbase + m_cache_ptr) & 0x0fffU);
			}
		}

		sio_step();
		pio_step();
	}
}

inline void dsp16_device_base::overlap_rom_data_read()
{
	DSP16_REQUIRE(cache::EXECUTE == m_cache_mode);
	DSP16_REQUIRE(phase::OP1 == m_phase);
	u16 const op(m_cache[m_cache_ptr]);
	switch (op >> 11)
	{
	case 0x19: // F1 ; y = a0 ; x = *pt++[i]
	case 0x1b: // F1 ; y = a1 ; x = *pt++[i]
	case 0x1f: // F1 ; y = Y ; x = *pt++[i]
		m_rom_data = program_read(m_core->xaau_pt);
		break;
	}
}

inline void dsp16_device_base::yaau_short_immediate_load(u16 op)
{
	u16 const r((op >> 9) & 0x0007U);
	u16 const m(op & 0x01ff);
	switch (r)
	{
	case 0x0: // j
		m_core->yaau_j = m | ((m & m_core->yaau_sign) ? ~m_core->yaau_mask : 0);
		break;
	case 0x1: // k
		m_core->yaau_k = m | ((m & m_core->yaau_sign) ? ~m_core->yaau_mask : 0);
		break;
	case 0x2: // rb
		m_core->yaau_rb = m;
		break;
	case 0x3: // re
		m_core->yaau_re = m;
		break;
	case 0x4: // r0
	case 0x5: // r1
	case 0x6: // r2
	case 0x7: // r3
		m_core->yaau_r[r & 0x0003U] = m;
		break;
	}
}

template <bool Debugger> inline s16 dsp16_device_base::yaau_read(u16 op)
{
	u16 const &r(m_core->op_yaau_r(op));
	s16 const result(Debugger ? m_workram[r & m_workram_mask] : m_workram[r & m_workram_mask]);
	m_core->yaau_postmodify_r(op);
	return result;
}

template <bool Debugger> inline void dsp16_device_base::yaau_write(u16 op, s16 value)
{
	u16 const &r(m_core->op_yaau_r(op));
	if (Debugger)
		m_workram[r & m_workram_mask] = value;
	else
		m_workram[r & m_workram_mask] = value;
	m_core->yaau_postmodify_r(op);
}

template <bool Debugger> void dsp16_device_base::yaau_write_z(u16 op)
{
	u16 &r(m_core->op_yaau_r(op));
	if (Debugger)
		m_workram[r & m_workram_mask] = m_core->dau_temp;
	else
		m_workram[r & m_workram_mask] = m_core->dau_temp;
	switch (op & 0x0003U)
	{
	case 0x0: // *rNzp
		if (m_core->yaau_re && (m_core->yaau_re == r))
			r = m_core->yaau_rb;
		else
			++r;
		break;
	case 0x1: // *rNpz
		break;
	case 0x2: // *rNm2
		r += 2;
		break;
	case 0x3: // *rNjk;
		r += m_core->yaau_k;
		break;
	}
	r &= m_core->yaau_mask;
}

/***********************************************************************
    built-in peripherals
***********************************************************************/

inline void dsp16_device_base::sio_step()
{
	// step the serial I/O clock divider
	if (m_sio_clk_div)
	{
		--m_sio_clk_div;
	}
	else
	{
		bool const active(!m_sio_clk);
		m_sio_clk = active ? 1U : 0U;
		if (sio_ick_active())
		{
			if (active)
				sio_ick_active_edge();
			m_ick_cb(m_sio_clk);
		}
		if (sio_ock_active())
		{
			m_ock_cb(m_sio_clk);
			if (active)
				sio_ock_active_edge();
		}
		m_sio_clk_div = m_sio_clk_res;
	}
}

inline void dsp16_device_base::pio_step()
{
	// udpate parallel input strobe
	if (m_pio_pids_cnt)
	{
		DSP16_REQUIRE(!m_pids_out);
		if (!--m_pio_pids_cnt)
		{
			if (!m_pio_r_cb.isunset())
				m_pio_pdx_in = m_pio_r_cb(m_psel_out, 0xffffU);
			m_pids_cb(m_pids_out = 1U);
			LOGPIO("DSP16: PIO read active edge PSEL = %u, PDX = %04X (PC = %04X)\n", m_psel_out, m_pio_pdx_in, m_st_pcbase);
		}
	}
	else
	{
		DSP16_REQUIRE(m_pids_out);
	}

	// udpate parallel output strobe
	if (m_pio_pods_cnt)
	{
		DSP16_REQUIRE(!m_pods_out);
		if (!--m_pio_pods_cnt)
		{
			LOGPIO("DSP16: PIO write active edge PSEL = %u, PDX = %04X (PC = %04X)\n", m_psel_out, m_pio_pdx_out, m_st_pcbase);
			m_pods_cb(1U);
			m_pio_w_cb(m_psel_out, m_pio_pdx_out, 0xffffU);
			m_pods_out = 1U;
			m_pdb_w_cb(m_psel_out, 0xffffU, 0x0000U);
		}
	}
	else
	{
		DSP16_REQUIRE(m_pods_out);
	}
}

/***********************************************************************
    inline helpers
***********************************************************************/

inline bool dsp16_device_base::op_interruptible(u16 op)
{
	switch (op >> 11)
	{
	case 0x02: // R = M
	case 0x03:
	case 0x04: // F1 ; Y = a1[l]
	case 0x05: // F1 ; Z : aT[l]
	case 0x06: // F1 ; Y
	case 0x07: // F1 ; aT[l] = Y
	case 0x08: // aT = R
	case 0x09: // R = a0
	case 0x0a: // R = N
	case 0x0b: // R = a1
	case 0x0c: // Y = R
	case 0x0d: // Z : R
	case 0x0f: // R = Y
	case 0x12: // ifc CON F2
	case 0x13: // if CON F2
	case 0x14: // F1 ; Y = y[l]
	case 0x15: // F1 ; Z : y[l]
	case 0x16: // F1 ; x = Y
	case 0x17: // F1 ; y[l] = Y
	case 0x19: // F1 ; y = a0 ; x = *pt++[i]
	case 0x1b: // F1 ; y = a1 ; x = *pt++[i]
	case 0x1c: // F1 ; Y = a0[l]
	case 0x1d: // F1 ; Z : y ; x = *pt++[i]
	case 0x1f: // F1 ; y = Y ; x = *pt++[i]
		return true;
	case 0x00: // goto JA
	case 0x01:
	case 0x0e: // do K { instr1...instrNI } # redo K
	case 0x10: // call JA
	case 0x11:
	case 0x18: // goto B
	case 0x1a: // if CON # icall
	case 0x1e: // Reserved
	default:
		return false;
	};
}

inline bool dsp16_device_base::check_predicate()
{
	bool const result(FLAGS_PRED_FALSE != (m_flags & FLAGS_PRED_MASK));
	set_predicate(FLAGS_PRED_NONE);
	return result;
}

inline u16 &dsp16_device_base::set_xaau_pc_offset(u16 offset)
{
	m_core->xaau_pc = (m_core->xaau_pc & XAAU_I_EXT) | (offset & XAAU_I_MASK);
	if (m_iack_out)
		m_core->xaau_pi = m_core->xaau_pc;
	return m_core->xaau_pc;
}

inline s16 dsp16_device_base::get_r(u16 op)
{
	switch (op_r(op))
	{
	case 0x00: // r0 (u)
	case 0x01: // r1 (u)
	case 0x02: // r2 (u)
	case 0x03: // r3 (u)
		return m_core->yaau_r[op_r(op)];
	case 0x04: // j (s)
		return m_core->yaau_j;
	case 0x05: // k (s)
		return m_core->yaau_k;
	case 0x06: // rb (u)
		return m_core->yaau_rb;
	case 0x07: // re (u)
		return m_core->yaau_re;
	case 0x08: // pt
		return m_core->xaau_pt;
	case 0x09: // pr
		return m_core->xaau_pr;
	case 0x0a: // pi
		return m_core->xaau_pi;
	case 0x0b: // i (s)
		return m_core->xaau_i;
	case 0x10: // x
		return m_core->dau_x;
	case 0x11: // y
		return u16(u32(m_core->dau_y >> 16));
	case 0x12: // yl
		return u16(u32(m_core->dau_y));
		break;
	case 0x13: // auc (u)
		return m_core->dau_auc;
	case 0x14: // psw
		return m_core->dau_export_psw();
	case 0x15: // c0 (s)
	case 0x16: // c1 (s)
	case 0x17: // c2 (s)
		return m_core->dau_c[op_r(op) - 0x15];
	case 0x18: // sioc
		return m_sio_sioc;
	case 0x19: // srta
		throw emu_fatalerror("DSP16: unimplemented get SRTA\n");
		return 0;
	case 0x1a: // sdx
		throw emu_fatalerror("DSP16: unimplemented get SDX\n");
		return 0;
	case 0x1b: // tdms
		throw emu_fatalerror("DSP16: unimplemented get TDMS\n");
		return 0;
	case 0x1c: // pioc
		return m_pio_pioc;
	case 0x1d: // pdx0
	case 0x1e: // pdx1
		return pio_pdx_read(BIT(op_r(op), 1));
	default:
		throw emu_fatalerror("DSP16: invalid R value %02X (PC = %04X)\n", op_r(op), m_st_pcbase);
	}
}

inline void dsp16_device_base::set_r(u16 op, s16 value)
{
	switch (op_r(op))
	{
	case 0x00: // r0 (u)
	case 0x01: // r1 (u)
	case 0x02: // r2 (u)
	case 0x03: // r3 (u)
		m_core->yaau_r[op_r(op)] = value & m_core->yaau_mask;
		break;
	case 0x04: // j (s)
		m_core->yaau_set_j(value);
		break;
	case 0x05: // k (s)
		m_core->yaau_set_k(value);
		break;
	case 0x06: // rb (u)
		m_core->yaau_set_rb(value);
		break;
	case 0x07: // re (u)
		m_core->yaau_set_re(value);
		break;
	case 0x08: // pt
		m_core->xaau_pt = value;
		break;
	case 0x09: // pr
		m_core->xaau_pr = value;
		break;
	case 0x0a: // pi
		// FIXME: reset PRNG
		if (!m_iack_out)
			m_core->xaau_pi = value;
		break;
	case 0x0b: // i (s)
		m_core->xaau_i = (value & XAAU_I_MASK) | ((value & XAAU_I_SIGN) ? XAAU_I_EXT : 0);
		break;
	case 0x10: // x
		m_core->dau_x = value;
		break;
	case 0x11: // y
		m_core->dau_set_y(value);
		break;
	case 0x12: // yl
		m_core->dau_set_yl(value);
		break;
	case 0x13: // auc (u)
		m_core->dau_auc = u8(value & 0x007f);
		break;
	case 0x14: // psw
		m_core->dau_psw = value;
		m_core->dau_import_psw();
		break;
	case 0x15: // c0 (s)
	case 0x16: // c1 (s)
	case 0x17: // c2 (s)
		m_core->dau_c[op_r(op) - 0x15] = u8(u16(value));
		break;
	case 0x18: // sioc
		sio_sioc_write(value);
		break;
	case 0x19: // srta
		throw emu_fatalerror("DSP16: unimplemented SRTA = %04X\n", value);
		break;
	case 0x1a: // sdx
		LOGSIO("DSP16: write SDX = %04X%s (PC = %04X)\n", value, pio_obe_status() ? "" : " - overrun", m_st_pcbase);
		if (pio_obe_status())
			LOGINT("DSP16 clear OBE flag (PC = %04X)\n", m_st_pcbase);
		m_sio_obuf = value;
		m_pio_pioc &= ~(u16(1) << 3);
		break;
	case 0x1b: // tdms
		throw emu_fatalerror("DSP16: unimplemented TDMS = %04X\n", value);
		break;
	case 0x1c: // pioc
		pio_pioc_write(value);
		break;
	case 0x1d: // pdx0
	case 0x1e: // pdx1
		pio_pdx_write(BIT(op_r(op), 1), value);
		break;
	default:
		throw emu_fatalerror("DSP16: invalid R value %02X (PC = %04X)\n", op_r(op), m_st_pcbase);
	}
}

s64 dsp16_device_base::dau_saturate(u16 a) const
{
	if (m_core->dau_auc_sat(a))
		return m_core->dau_a[a];
	else
		return std::clamp<s64>(m_core->dau_a[a], std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max());
}

inline bool dsp16_device_base::op_dau_con(u16 op, bool inc)
{
	bool result;
	u16 const con(op_con(op));
	switch (con >> 1)
	{
	case 0x0: // mi/pl
		result = m_core->dau_psw_lmi();
		break;
	case 0x1: // eq/ne
		result = m_core->dau_psw_leq();
		break;
	case 0x2: // lvs/lvc
		result = m_core->dau_psw_llv();
		break;
	case 0x3: // mvs/mvc
		result = m_core->dau_psw_lmv();
		break;
	case 0x4: // heads/tails
		throw emu_fatalerror("DSP16: unimplemented CON value %02X (PC = %04X)\n", con, m_st_pcbase);
	case 0x5: // c0ge/c0lt
	case 0x6: // c1ge/c1lt
		{
			s8 &c(m_core->dau_c[(con >> 1) - 0x05]);
			result = 0 <= c;
			if (inc)
				++c;
		}
		break;
	case 0x7: // true/false
		result = true;
		break;
	case 0x8: // gt/le
		result = !m_core->dau_psw_lmi() && !m_core->dau_psw_leq();
		break;
	default: // Reserved
		throw emu_fatalerror("DSP16: reserved CON value %02X (PC = %04X)\n", con, m_st_pcbase);
	}
	return BIT(con, 0) ? !result : result;
}

/***********************************************************************
    serial I/O
***********************************************************************/

void dsp16_device_base::sio_sioc_write(u16 value)
{
	[[maybe_unused]] static constexpr unsigned DIVIDERS[4] = { 4U, 12U, 16U, 20U };
	value &= 0x03ffU;
	LOGSIO(
			"DSP16: write SIOC = %03X: "
			"LD %cCK/n->%cCK/n, CLK CKI/%u->CKI/%u, %cSB->%cSB first, "
			"OLD %s->%s, ILD %s->%s, OCK %s->%s, ICK %s->%s, OLEN %u->%u, ILEN %u->%u "
			"(PC = %04X)\n",
			value,
			sio_ld_ock() ? 'O' : 'I', BIT(value, 9) ? 'O' : 'I',
			DIVIDERS[(m_sio_sioc >> 7) & 0x0003U], DIVIDERS[(value >> 7) & 0x0003U],
			sio_msb_first() ? 'M' : 'L', BIT(value, 6) ? 'M' : 'L',
			sio_old_active() ? "active" : "passive", BIT(value, 5) ? "active" : "passive",
			sio_ild_active() ? "active" : "passive", BIT(value, 4) ? "active" : "passive",
			sio_ock_active() ? "active" : "passive", BIT(value, 3) ? "active" : "passive",
			sio_ick_active() ? "active" : "passive", BIT(value, 2) ? "active" : "passive",
			sio_olen(), BIT(value, 1) ? 8U : 16U,
			sio_ilen(), BIT(value, 0) ? 8U : 16U,
			m_st_pcbase);

	bool const old_change(BIT(value ^ m_sio_sioc, 5));
	bool const ild_change(BIT(value ^ m_sio_sioc, 4));
	bool const ock_change(BIT(value ^ m_sio_sioc, 3));
	bool const ick_change(BIT(value ^ m_sio_sioc, 2));
	m_sio_sioc = value;

	// we're using treating high-impedance and high as the same thing
	if (!m_sio_ld)
	{
		if (ild_change)
			m_ild_cb(sio_ild_active() ? 0 : 1);
		if (old_change)
			m_old_cb(sio_old_active() ? 0 : 1);
	}
	if (!m_sio_clk)
	{
		if (ick_change)
			m_ick_cb(sio_ick_active() ? 0 : 1);
		if (ock_change)
			m_ock_cb(sio_ock_active() ? 0 : 1);
	}

	// precalculate divider preload
	switch ((value >> 7) & 0x0003U)
	{
	case 0x0: m_sio_clk_res = (4 / 4) - 1; break; // CKI÷4
	case 0x1: m_sio_clk_res = (12 / 4) - 1; break; // CKI÷12
	case 0x2: m_sio_clk_res = (16 / 4) - 1; break; // CKI÷16
	case 0x3: m_sio_clk_res = (20 / 4) - 1; break; // CKI÷20
	}
}

inline void dsp16_device_base::sio_ick_active_edge()
{
	// check for start of transaction
	bool const ild(sio_ild_active() ? !m_sio_ld : (CLEAR_LINE != m_ild_in));
	if (ASSERT_LINE != m_ild_in)
		m_ild_in = CLEAR_LINE;
	if (ild)
	{
		if (!unsupported_rx_warned) { logerror("DSP16: serial input frame ignored; RX data is unimplemented\n"); unsupported_rx_warned = true; }
		m_sio_flags |= SIO_FLAGS_ILD;
	}
	else
	{
		m_sio_flags &= ~SIO_FLAGS_ILD;
	}

	// update internal LD divider if it's being driven by ICK
	if (sio_ld_ick())
		sio_step_ld_div();
}

inline void dsp16_device_base::sio_ock_active_edge()
{
	// check for load opportunity
	bool const old(sio_old_active() ? !m_sio_ld : (CLEAR_LINE != m_old_in));
	if (ASSERT_LINE != m_old_in)
		m_old_in = CLEAR_LINE;
	if (old)
	{
		if ((SIO_FLAGS_NONE == (m_sio_flags & SIO_FLAGS_OLD)) && !m_sio_ofsr && !pio_obe_status())
		{
			bool const eight_bit(8U == sio_olen());
			LOGSIO(
					"DSP16: load OSR = %04X & %04X %cSB first (PC = %04X)\n",
					m_sio_obuf, !eight_bit ? 0xffffU : sio_msb_first() ? 0xff00U : 0x00ffU, sio_msb_first() ? 'M' : 'L', m_st_pcbase);
			LOGINT("DSP16: set OBE flag (PC = %04X)\n", m_st_pcbase);

			m_sio_osr = sio_msb_first() ? bitswap<16>(m_sio_obuf, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15) : m_sio_obuf;
			m_sio_ofsr = 0xffffU;
			if (eight_bit)
			{
				m_sio_osr |= 0xff00U;
				m_sio_ofsr >>= 8;
			}

			m_pio_pioc |= u16(1) << 3;
		}
		m_sio_flags |= SIO_FLAGS_OLD;
	}
	else
	{
		m_sio_flags &= ~SIO_FLAGS_OLD;
	}

	// shift out serial data
	if (BIT(m_sio_osr, 0) != m_do_out)
		m_do_cb(m_do_out = BIT(m_sio_osr, 0));
	if (BIT(~m_sio_ofsr, 0) != m_ose_out)
	{
		LOGSIO("DSP16: OSE %u->%u (PC = %04X)\n", m_ose_out, BIT(~m_sio_ofsr, 0), m_st_pcbase);
		m_ose_cb(m_ose_out = BIT(~m_sio_ofsr, 0));
	}
	m_sio_osr = (m_sio_osr >> 1) | 0x8000U;
	m_sio_ofsr >>= 1;

	// update internal LD divider if it's being driven by OCK
	if (sio_ld_ock())
		sio_step_ld_div();
}

inline void dsp16_device_base::sio_step_ld_div()
{
	if (m_sio_ld_div)
	{
		--m_sio_ld_div;
	}
	else
	{
		m_sio_ld = m_sio_ld ? 0U : 1U;
		m_sio_ld_div = (16 / 2) - 1;
		if (sio_ild_active())
			m_ild_cb(m_sio_ld);
		if (sio_old_active())
			m_old_cb(m_sio_ld);
	}
}

/***********************************************************************
    parallel I/O
***********************************************************************/

void dsp16_device_base::pio_pioc_write(u16 value)
{
	LOGPIO(
			"DSP16: write PIOC = %04X: "
			"STROBE %uT->%uT, PODS %s->%s, PIDS %s->%s, S/C %u->%u, "
			"IBF %s->%s, OBE %s->%s, PIDS %s->%s, PODS %s->%s, INT %s->%s "
			"(PC = %04X)\n",
			value,
			pio_strobe(), ((value >> 13) & 0x0003U) + 1,
			pio_pods_active() ? "active" : "passive", BIT(value, 12) ? "active" : "passive",
			pio_pids_active() ? "active" : "passive", BIT(value, 11) ? "active" : "passive",
			BIT(m_pio_pioc, 10), BIT(value, 10),
			pio_ibf_enable() ? "en" : "dis", BIT(value, 9) ? "en" : "dis",
			pio_obe_enable() ? "en" : "dis", BIT(value, 8) ? "en" : "dis",
			pio_pids_enable() ? "en" : "dis", BIT(value, 7) ? "en" : "dis",
			pio_pods_enable() ? "en" : "dis", BIT(value, 6) ? "en" : "dis",
			pio_int_enable() ? "en" : "dis", BIT(value, 5) ? "en" : "dis",
			m_st_pcbase);

	if (BIT(value, 10)) throw emu_fatalerror("DSP16: PIO S/C mode is unimplemented\n");
	m_pio_pioc = (m_pio_pioc & 0x801fU) | (value & 0x7fe0U);
	m_int_enable[1] = (value >> 5) & 0x001fU;
	if (!pio_pids_active())
	{
		m_pio_pids_cnt = 0U;
		if (!m_pids_out)
			m_pids_cb(m_pids_out = 1U); // actually high-impedance
	}
	if (!pio_pods_active())
	{
		m_pio_pods_cnt = 0U;
		if (!m_pods_out)
		{
			m_pods_cb(m_pods_out = 1U); // actually high-impedance
			m_pdb_w_cb(m_psel_out, 0xffffU, 0x0000U);
		}
	}
}

u16 dsp16_device_base::pio_pdx_read(u16 sel)
{
	bool const active(pio_pids_active());
	LOGPIO("DSP16: read PDX%u = %04X %s (PC = %04X)\n", sel, m_pio_pdx_in, active ? "active" : "passive", m_st_pcbase);
	if (pio_pids_status())
		LOGINT("DSP16: clear PIDS flag (PC = %04X)\n", m_st_pcbase);
	m_pio_pioc &= ~(u16(1) << 2);

	if (sel != m_psel_out)
	{
		LOGPIO("DSP16: PSEL %u->%u (PC = %04X)\n", m_psel_out, sel, m_st_pcbase);
		m_psel_cb(m_psel_out = sel);
	}

	if (active)
	{
		if (m_pio_pids_cnt)
		{
			DSP16_REQUIRE(!m_pids_out);
			logerror("DSP16: PDX%u active read while PIDS still asserted (PC = %04X)\n", sel, m_st_pcbase);
		}
		else
		{
			DSP16_REQUIRE(m_pids_out);
			m_pids_cb(m_pids_out = 0U);
		}
		m_pio_pids_cnt = pio_strobe() + 1; // decremented this cycle
	}

	return m_pio_pdx_in;
}

void dsp16_device_base::pio_pdx_write(u16 sel, u16 value)
{
	bool const active(pio_pods_active());
	LOGPIO("DSP16: write PDX%u = %04X %s (PC = %04X)\n", sel, value, active ? "active" : "passive", m_st_pcbase);
	if (pio_pods_status())
		LOGINT("DSP16: clear PODS flag (PC = %04X)\n", m_st_pcbase);
	m_pio_pioc &= ~(u16(1) << 1);
	m_pio_pdx_out = value;

	if (sel != m_psel_out)
	{
		LOGPIO("DSP16: PSEL %u->%u (PC = %04X)\n", m_psel_out, sel, m_st_pcbase);
		m_psel_cb(m_psel_out = sel);
	}

	if (active)
	{
		if (m_pio_pods_cnt)
		{
			DSP16_REQUIRE(!m_pods_out);
			logerror("DSP16: PDX%u active write while PODS still asserted (PC = %04X)\n", sel, m_st_pcbase);
		}
		else
		{
			DSP16_REQUIRE(m_pods_out);
			m_pods_cb(m_pods_out = 0U);
			m_pdb_w_cb(sel, value, 0xffffU);
		}
		m_pio_pods_cnt = pio_strobe() + 1; // decremented this cycle
	}
}


