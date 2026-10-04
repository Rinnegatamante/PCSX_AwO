// Generated from MAME; license:BSD-3-Clause
// copyright-holders:R. Belmont, Karl Stenerud
const int m37710_cpu_device::m37710_irq_vectors[M37710_INTERRUPT_MAX] =
{
	// maskable
	0xffce, // DMA3
	0xffd0, // DMA2
	0xffd2, // DMA1
	0xffd4, // DMA0
	0xffd6, // A-D converter
	0xffd8, // UART1 transmit
	0xffda, // UART1 receive
	0xffdc, // UART0 transmit
	0xffde, // UART0 receive
	0xffe0, // Timer B2
	0xffe2, // Timer B1
	0xffe4, // Timer B0
	0xffe6, // Timer A4
	0xffe8, // Timer A3
	0xffea, // Timer A2
	0xffec, // Timer A1
	0xffee, // Timer A0
	0xfff0, // external INT2 pin
	0xfff2, // external INT1 pin
	0xfff4, // external INT0 pin

	// non-maskable
	0xfff6, // watchdog timer
	0xfff8, // debugger control (not used in shipping ICs?)
	0xfffa, // BRK
	0xfffc, // divide by zero
	0xfffe, // RESET
};

