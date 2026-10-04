#include "psx_sio.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Trace {
	psx_sio_t *sio;
	int irq[64];
	size_t irq_count;
	int sck[128];
	uint64_t sck_cycle[128];
	size_t sck_count;
	int txd[128];
	uint64_t txd_cycle[128];
	size_t txd_count;
	int dtr[32];
	size_t dtr_count;
	int rts[32];
	size_t rts_count;
} Trace;

static void irq(void *opaque, int state)
{
	Trace *t = (Trace *)opaque;
	assert(t->irq_count < sizeof(t->irq) / sizeof(t->irq[0]));
	t->irq[t->irq_count++] = state;
}

static void sck(void *opaque, int state)
{
	Trace *t = (Trace *)opaque;
	assert(t->sck_count < sizeof(t->sck) / sizeof(t->sck[0]));
	t->sck[t->sck_count++] = state;
	t->sck_cycle[t->sck_count - 1] = psx_sio_elapsed_cycles(t->sio);
}

static void txd(void *opaque, int state)
{
	Trace *t = (Trace *)opaque;
	assert(t->txd_count < sizeof(t->txd) / sizeof(t->txd[0]));
	t->txd[t->txd_count++] = state;
	t->txd_cycle[t->txd_count - 1] = psx_sio_elapsed_cycles(t->sio);
}

static void dtr(void *opaque, int state)
{
	Trace *t = (Trace *)opaque;
	assert(t->dtr_count < sizeof(t->dtr) / sizeof(t->dtr[0]));
	t->dtr[t->dtr_count++] = state;
}

static void rts(void *opaque, int state)
{
	Trace *t = (Trace *)opaque;
	assert(t->rts_count < sizeof(t->rts) / sizeof(t->rts[0]));
	t->rts[t->rts_count++] = state;
}

static psx_sio_t *make_sio(psx_sio_variant_t variant, Trace *trace)
{
	psx_sio_callbacks_t callbacks;
	psx_sio_t *sio = NULL;
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.irq = irq;
	callbacks.sck = sck;
	callbacks.txd = txd;
	callbacks.dtr = dtr;
	callbacks.rts = rts;
	callbacks.opaque = trace;
	assert(psx_sio_create(variant, &callbacks, &sio) == PSX_SIO_OK);
	assert(sio != NULL);
	if (trace != NULL)
		trace->sio = sio;
	return sio;
}

static void read_word(psx_sio_t *sio, uint32_t offset, uint32_t *value)
{
	assert(psx_sio_read(sio, offset, 0xffffffffu, value) == PSX_SIO_OK);
}

static void write_word(psx_sio_t *sio, uint32_t offset, uint32_t value,
		uint32_t mask)
{
	assert(psx_sio_write(sio, offset, value, mask) == PSX_SIO_OK);
}

static void set_mode_control(psx_sio_t *sio, uint16_t mode, uint16_t control)
{
	write_word(sio, 2, (uint32_t)control << 16 | mode, 0xffffffffu);
}

static void set_baud(psx_sio_t *sio, uint16_t baud)
{
	write_word(sio, 3, (uint32_t)baud << 16, 0xffff0000u);
}

static void test_register_layout_and_mem_masks(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	uint32_t value;
	read_word(sio, 1, &value);
	assert(value == (PSX_SIO_STATUS_TX_RDY | PSX_SIO_STATUS_TX_EMPTY));

	/* One accessed bit in each MAME halfword lane updates that whole register. */
	write_word(sio, 2, 0xabcd0003u, 0x00000001u);
	write_word(sio, 2, 0x000010000000u, 0x00010000u);
	write_word(sio, 3, 0x77770000u, 0x000000ffu);
	read_word(sio, 2, &value);
	assert(value == 0x10000003u);
	read_word(sio, 3, &value);
	assert(value == 0);
	write_word(sio, 3, 0x00070000u, 0x00010000u);
	read_word(sio, 3, &value);
	assert(value == 0x00070000u);
	assert(trace.rts_count == 0 && trace.sck_count == 0 && trace.txd_count == 0);
	psx_sio_destroy(sio);
}

static void test_sio0_lsb_tx_rx_and_iack(void)
{
	static const int expected_tx[8] = {1, 1, 0, 0, 1, 1, 0, 1};
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	uint32_t value;
	unsigned bit;
	set_mode_control(sio, 1, PSX_SIO_CONTROL_TX_ENA |
		PSX_SIO_CONTROL_TX_IENA | PSX_SIO_CONTROL_RX_IENA);
	set_baud(sio, 3);
	/* The source ignores the TX data register's lane mask. */
	write_word(sio, 4, 0xb3, 0);
	read_word(sio, 1, &value);
	assert((value & (PSX_SIO_STATUS_TX_RDY | PSX_SIO_STATUS_TX_EMPTY)) == 0);
	assert(psx_sio_advance_to_cycle(sio, 2) == PSX_SIO_OK);
	assert(trace.txd_count == 0 && trace.sck_count == 0);
	for (bit = 0; bit < 8; bit++) {
		psx_sio_write_rxd(sio, (0x62 >> bit) & 1);
		assert(psx_sio_advance_to_cycle(sio, (bit + 1) * 3) == PSX_SIO_OK);
	}
	assert(trace.txd_count == 8 && trace.sck_count == 16);
	for (bit = 0; bit < 8; bit++) {
		assert(trace.txd[bit] == expected_tx[bit]);
		assert(trace.sck[bit * 2] == 0 && trace.sck[bit * 2 + 1] == 1);
		assert(trace.txd_cycle[bit] == (uint64_t)(bit + 1) * 3);
		assert(trace.sck_cycle[bit * 2] == (uint64_t)(bit + 1) * 3);
		assert(trace.sck_cycle[bit * 2 + 1] == (uint64_t)(bit + 1) * 3);
	}
	assert(trace.irq_count == 2 && trace.irq[0] == 1 && trace.irq[1] == 1);
	read_word(sio, 1, &value);
	assert((value & (PSX_SIO_STATUS_RX_RDY | PSX_SIO_STATUS_TX_EMPTY |
		PSX_SIO_STATUS_TX_RDY | PSX_SIO_STATUS_IRQ)) ==
		(PSX_SIO_STATUS_RX_RDY | PSX_SIO_STATUS_TX_EMPTY |
		PSX_SIO_STATUS_TX_RDY | PSX_SIO_STATUS_IRQ));
	read_word(sio, 0, &value);
	assert(value == 0x62);
	read_word(sio, 1, &value);
	assert((value & PSX_SIO_STATUS_RX_RDY) == 0);
	read_word(sio, 0, &value);
	assert(value == 0xff);

	write_word(sio, 2, (uint32_t)PSX_SIO_CONTROL_IACK << 16,
		0xffff0000u);
	read_word(sio, 1, &value);
	assert((value & PSX_SIO_STATUS_IRQ) == 0);
	read_word(sio, 2, &value);
	assert((value >> 16) == 0);
	assert(trace.irq_count == 3 && trace.irq[2] == 0);
	assert(trace.rts_count == 0);
	psx_sio_destroy(sio);
}

static void test_overrun_preserves_first_byte(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	uint32_t value;
	unsigned bit;
	set_mode_control(sio, 1, PSX_SIO_CONTROL_TX_ENA);
	set_baud(sio, 2);
	write_word(sio, 0, 0x55, 0xffffffffu);
	for (bit = 0; bit < 8; bit++) {
		psx_sio_write_rxd(sio, (0x62 >> bit) & 1);
		assert(psx_sio_advance_to_cycle(sio, (bit + 1) * 2) == PSX_SIO_OK);
	}
	write_word(sio, 0, 0xaa, 0xffffffffu);
	for (; bit < 16; bit++) {
		psx_sio_write_rxd(sio, (0x9d >> (bit - 8)) & 1);
		assert(psx_sio_advance_to_cycle(sio, (bit + 1) * 2) == PSX_SIO_OK);
	}
	read_word(sio, 1, &value);
	assert((value & (PSX_SIO_STATUS_RX_RDY | PSX_SIO_STATUS_OVERRUN)) ==
		(PSX_SIO_STATUS_RX_RDY | PSX_SIO_STATUS_OVERRUN));
	read_word(sio, 0, &value);
	assert(value == 0x62);
	psx_sio_destroy(sio);
}

static void test_prescaler_sio1_and_disabled_pins(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO1, &trace);
	uint32_t value;
	unsigned bit;
	set_mode_control(sio, 2, PSX_SIO_CONTROL_TX_ENA |
		PSX_SIO_CONTROL_TX_IENA | PSX_SIO_CONTROL_RX_IENA);
	set_baud(sio, 2); /* 16 x baud: one bit every 32 PSX cycles. */
	write_word(sio, 0, 0xb3, 0xffffffffu);
	assert(psx_sio_advance_to_cycle(sio, 31) == PSX_SIO_OK);
	assert(trace.txd_count == 0);
	for (bit = 0; bit < 8; bit++) {
		psx_sio_write_rxd(sio, 0);
		assert(psx_sio_advance_to_cycle(sio, (bit + 1) * 32) == PSX_SIO_OK);
	}
	assert(trace.txd_count == 8 && trace.sck_count == 0);
	for (bit = 0; bit < 8; bit++)
		assert(trace.txd[bit] == ((0xb3 >> bit) & 1));
	read_word(sio, 1, &value);
	assert((value & PSX_SIO_STATUS_IRQ) != 0);
	assert((value & PSX_SIO_STATUS_RX_RDY) == 0); /* MAME only clocks RX on SIO0. */
	assert(trace.irq_count == 1 && trace.irq[0] == 1);
	psx_sio_destroy(sio);

	memset(&trace, 0, sizeof(trace));
	sio = make_sio(PSX_SIO0, &trace);
	set_mode_control(sio, 1, 0); /* TX_ENA clear. */
	set_baud(sio, 1);
	write_word(sio, 0, 0x96, 0xffffffffu);
	assert(psx_sio_advance_to_cycle(sio, 100) == PSX_SIO_OK);
	assert(trace.sck_count == 0 && trace.txd_count == 0);
	read_word(sio, 1, &value);
	assert((value & PSX_SIO_STATUS_TX_EMPTY) == 0);
	psx_sio_destroy(sio);
}

static void test_reset_and_dsr_edge(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	uint32_t value;
	set_mode_control(sio, 3, PSX_SIO_CONTROL_TX_ENA | PSX_SIO_CONTROL_DSR_IENA |
		PSX_SIO_CONTROL_DTR);
	set_baud(sio, 2);
	psx_sio_write_dsr(sio, 1);
	psx_sio_write_dsr(sio, 0);
	psx_sio_write_dsr(sio, 0);
	assert(trace.irq_count == 1 && trace.irq[0] == 1);
	read_word(sio, 1, &value);
	assert(value & PSX_SIO_STATUS_DSR);
	psx_sio_write_dsr(sio, 1);
	read_word(sio, 1, &value);
	assert((value & PSX_SIO_STATUS_DSR) == 0);

	write_word(sio, 0, 0x33, 0xffffffffu);
	assert(psx_sio_advance_to_cycle(sio, 128) == PSX_SIO_OK);
	assert(trace.sck_count == 2 && trace.txd_count == 1);
	write_word(sio, 2, (uint32_t)PSX_SIO_CONTROL_RESET << 16,
		0xffff0000u);
	assert(trace.dtr_count == 4); /* initial write, then reset's 0/1 and DTR=1 */
	assert(trace.dtr[0] == 0 && trace.dtr[1] == 0 && trace.dtr[2] == 1 && trace.dtr[3] == 1);
	assert(trace.txd[trace.txd_count - 1] == 1);
	read_word(sio, 2, &value);
	assert((value & 0xffffu) == 3 && (value >> 16) == PSX_SIO_CONTROL_RESET);
	read_word(sio, 1, &value);
	assert((value & (PSX_SIO_STATUS_TX_EMPTY | PSX_SIO_STATUS_TX_RDY)) ==
		(PSX_SIO_STATUS_TX_EMPTY | PSX_SIO_STATUS_TX_RDY));
	assert((value & (PSX_SIO_STATUS_DSR | PSX_SIO_STATUS_IRQ)) == 0);
	assert(trace.rts_count == 0);
	psx_sio_destroy(sio);
}

static void test_data_register_overwrite_uses_shift_snapshot(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	unsigned bit;
	set_mode_control(sio, 1, PSX_SIO_CONTROL_TX_ENA);
	set_baud(sio, 2);
	write_word(sio, 0, 0xb3, 0xffffffffu);
	assert(psx_sio_advance_to_cycle(sio, 2) == PSX_SIO_OK);
	write_word(sio, 0, 0x62, 0xffffffffu);
	for (bit = 1; bit < 9; bit++)
		assert(psx_sio_advance_to_cycle(sio, bit * 2) == PSX_SIO_OK);
	assert(trace.txd_count == 8);
	for (bit = 0; bit < 8; bit++)
		assert(trace.txd[bit] == ((0xb3 >> bit) & 1));
	assert(psx_sio_advance_to_cycle(sio, 18) == PSX_SIO_OK);
	assert(trace.txd_count == 9 && trace.txd[8] == 0);
	psx_sio_destroy(sio);
}

static void test_callbacks_see_each_timer_deadline_during_catchup(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	unsigned i;
	set_mode_control(sio, 1, PSX_SIO_CONTROL_TX_ENA);
	set_baud(sio, 1);
	write_word(sio, 0, 0xb3, 0xffffffffu);
	assert(psx_sio_advance_to_cycle(sio, 8) == PSX_SIO_OK);
	assert(trace.txd_count == 8 && trace.sck_count == 16);
	for (i = 0; i < 8; i++) {
		assert(trace.txd_cycle[i] == i + 1);
		assert(trace.sck_cycle[2 * i] == i + 1);
		assert(trace.sck_cycle[2 * i + 1] == i + 1);
	}
	assert(psx_sio_elapsed_cycles(sio) == 8);
	psx_sio_destroy(sio);
}

static void test_time_errors_and_baud_arm_order(void)
{
	Trace trace = {0};
	psx_sio_t *sio = make_sio(PSX_SIO0, &trace);
	assert(psx_sio_advance_to_cycle(sio, 10) == PSX_SIO_OK);
	assert(psx_sio_advance_to_cycle(sio, 9) == PSX_SIO_TIME_REWIND);
	/* Writing data before mode/baud leaves MAME's invalid timer stopped. */
	write_word(sio, 0, 0x31, 0xffffffffu);
	set_mode_control(sio, 1, PSX_SIO_CONTROL_TX_ENA);
	set_baud(sio, 1);
	assert(psx_sio_advance_to_cycle(sio, 100) == PSX_SIO_OK);
	assert(trace.txd_count == 0 && trace.sck_count == 0);
	psx_sio_destroy(sio);

	memset(&trace, 0, sizeof(trace));
	sio = make_sio(PSX_SIO0, &trace);
	assert(psx_sio_advance_to_cycle(sio, UINT64_MAX - 1) == PSX_SIO_OK);
	set_mode_control(sio, 1, PSX_SIO_CONTROL_TX_ENA);
	set_baud(sio, 3);
	assert(psx_sio_write(sio, 0, 0xaa, 0xffffffffu) == PSX_SIO_TIME_OVERFLOW);
	psx_sio_destroy(sio);
}

int main(void)
{
	psx_sio_t *sio = NULL;
	assert(psx_sio_create((psx_sio_variant_t)2, NULL, &sio) == PSX_SIO_ARGUMENT);
	assert(sio == NULL);
	test_register_layout_and_mem_masks();
	test_sio0_lsb_tx_rx_and_iack();
	test_overrun_preserves_first_byte();
	test_prescaler_sio1_and_disabled_pins();
	test_reset_and_dsr_edge();
	test_data_register_overwrite_uses_shift_snapshot();
	test_callbacks_see_each_timer_deadline_during_catchup();
	test_time_errors_and_baud_arm_order();
	puts("PSX SIO0/SIO1 registers, lanes, exact serial timers, IRQ, DSR and control reset PASS");
	return 0;
}
