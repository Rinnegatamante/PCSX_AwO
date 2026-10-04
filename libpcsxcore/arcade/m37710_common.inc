// Generated from MAME; license:BSD-3-Clause
// copyright-holders:R. Belmont, Karl Stenerud
const m37710_cpu_device::opcode_func *const m37710_cpu_device::m37710i_opcodes[4] =
{
	m37710i_opcodes_M0X0,
	m37710i_opcodes_M0X1,
	m37710i_opcodes_M1X0,
	m37710i_opcodes_M1X1,
};

const m37710_cpu_device::opcode_func *const m37710_cpu_device::m37710i_opcodes2[4] =
{
	m37710i_opcodes42_M0X0,
	m37710i_opcodes42_M0X1,
	m37710i_opcodes42_M1X0,
	m37710i_opcodes42_M1X1,
};

const m37710_cpu_device::opcode_func *const m37710_cpu_device::m37710i_opcodes3[4] =
{
	m37710i_opcodes89_M0X0,
	m37710i_opcodes89_M0X1,
	m37710i_opcodes89_M1X0,
	m37710i_opcodes89_M1X1,
};

const m37710_cpu_device::get_reg_func m37710_cpu_device::m37710i_get_reg[4] =
{
	&m37710_cpu_device::m37710i_get_reg_M0X0,
	&m37710_cpu_device::m37710i_get_reg_M0X1,
	&m37710_cpu_device::m37710i_get_reg_M1X0,
	&m37710_cpu_device::m37710i_get_reg_M1X1,
};

const m37710_cpu_device::set_reg_func m37710_cpu_device::m37710i_set_reg[4] =
{
	&m37710_cpu_device::m37710i_set_reg_M0X0,
	&m37710_cpu_device::m37710i_set_reg_M0X1,
	&m37710_cpu_device::m37710i_set_reg_M1X0,
	&m37710_cpu_device::m37710i_set_reg_M1X1,
};

const m37710_cpu_device::execute_func m37710_cpu_device::m37710i_execute[4] =
{
	&m37710_cpu_device::m37710i_execute_M0X0,
	&m37710_cpu_device::m37710i_execute_M0X1,
	&m37710_cpu_device::m37710i_execute_M1X0,
	&m37710_cpu_device::m37710i_execute_M1X1,
};

/* internal functions */

void m37710_cpu_device::m37710i_update_irqs()
{
	int curirq, pending = LINE_IRQ;
	int wantedIRQ = -1;
	int curpri = 0;

	for (curirq = M37710_INTERRUPT_MAX - 1; curirq >= 0; curirq--)
	{
		if ((pending & (1 << curirq)))
		{
			// this IRQ is set
			if (curirq < M37710_MASKABLE_INTERRUPTS)
			{
				int control = m_int_control[curirq];
				int thispri = control & 7;
				// logerror("line %d set, level %x curpri %x IPL %x\n", curirq, thispri, curpri, m_ipl);
				// it's maskable, check if the level works, also make sure it's acceptable for the current CPU level
				if (!FLAG_I && thispri > curpri && uint32_t(thispri) > m_ipl)
				{
					// mark us as the best candidate
					LOGMASKED(LOG_INT, "%s interrupt active with priority %d (PC=%x)\n", m37710_intnames[curirq], thispri, REG_PG | REG_PC);
					wantedIRQ = curirq;
					curpri = thispri;
				}
			}
			else
			{
				// non-maskable
				LOGMASKED(LOG_INT, "%s interrupt active (PC=%x)\n", m37710_intnames[curirq], REG_PG | REG_PC);
				wantedIRQ = curirq;
				curpri = 7;
				break;  // no more processing, NMIs always win
			}
		}
	}

	if (wantedIRQ != -1)
	{
		irq_ack(wantedIRQ);

		// make sure we're running to service the interrupt
		CPU_STOPPED &= ~STOP_LEVEL_WAI;

		// auto-clear line
		m37710_set_irq_line(wantedIRQ, CLEAR_LINE);

		// let's do it...
		// push PB, then PC, then status
		CLK(13);
		m37710i_push_8(REG_PG>>16);
		m37710i_push_16(REG_PC);
		m37710i_push_8(m_ipl);
		m37710i_push_8(m37710i_get_reg_ps());

		// set I to 1, set IPL to the interrupt we're taking
		FLAG_I = IFLAG_SET;
		m_ipl = curpri;
		// then PG=0, PC=(vector)
		REG_PG = 0;
		REG_PC = m37710_read_16(m37710_irq_vectors[wantedIRQ]);
	}
}

void m37710_cpu_device::m37710i_set_execution_mode(uint32_t mode)
{
	m_opcodes = m37710i_opcodes[mode];
	m_opcodes42 = m37710i_opcodes2[mode];
	m_opcodes89 = m37710i_opcodes3[mode];
	FTABLE_GET_REG = m37710i_get_reg[mode];
	FTABLE_SET_REG = m37710i_set_reg[mode];
	m_execute = m37710i_execute[mode];
}


/* ======================================================================== */
/* =============================== INTERRUPTS ============================= */
/* ======================================================================== */

void m37710_cpu_device::m37710i_interrupt_software(uint32_t vector)
{
	CLK(13);
	m37710i_push_8(REG_PG>>16);
	m37710i_push_16(REG_PC);
	m37710i_push_8(m_ipl);
	m37710i_push_8(m37710i_get_reg_ps());
	FLAG_I = IFLAG_SET;
	REG_PG = 0;
	REG_PC = m37710_read_16(vector);
}



