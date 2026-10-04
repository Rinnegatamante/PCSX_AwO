/* BSD-3-Clause; derived from MAME's PlayStation SIO, copyright smf 2003-2011. */
#include "psx_sio.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define PSX_SIO_CLOCK_HZ 33868800u

struct psx_sio {
	psx_sio_variant_t variant;
	psx_sio_callbacks_t callbacks;
	uint32_t status;
	uint32_t mode;
	uint32_t control;
	uint32_t baud;
	int rxd;
	uint32_t tx_data;
	uint32_t rx_data;
	uint32_t tx_shift;
	uint32_t rx_shift;
	uint32_t tx_bits;
	uint32_t rx_bits;
	uint64_t now_cycle;
	uint64_t deadline;
	uint64_t interval;
	int timer_pending;
};

static void call_line(void (*callback)(void *, int), psx_sio_t *sio, int state)
{
	if (callback != NULL)
		callback(sio->callbacks.opaque, state);
}

static void sio_interrupt(psx_sio_t *sio)
{
	sio->status |= PSX_SIO_STATUS_IRQ;
	call_line(sio->callbacks.irq, sio, 1);
}

static uint64_t prescaler(uint32_t mode)
{
	switch (mode & 3u) {
	case 1: return 1;
	case 2: return 16;
	case 3: return 64;
	default: return 0;
	}
}

static psx_sio_result_t sio_timer_adjust(psx_sio_t *sio)
{
	uint64_t divisor;
	if ((sio->status & PSX_SIO_STATUS_TX_EMPTY) != 0 && sio->tx_bits == 0) {
		sio->timer_pending = 0;
		return PSX_SIO_OK;
	}
	divisor = prescaler(sio->mode);
	if (sio->baud == 0 || divisor == 0) {
		sio->timer_pending = 0;
		return PSX_SIO_OK;
	}
	sio->interval = divisor * (uint64_t)sio->baud;
	if (sio->interval > UINT64_MAX - sio->now_cycle) {
		sio->timer_pending = 0;
		return PSX_SIO_TIME_OVERFLOW;
	}
	sio->deadline = sio->now_cycle + sio->interval;
	sio->timer_pending = 1;
	return PSX_SIO_OK;
}

static psx_sio_result_t sio_tick(psx_sio_t *sio)
{
	psx_sio_result_t result;
	if (sio->tx_bits == 0 &&
		(sio->control & PSX_SIO_CONTROL_TX_ENA) != 0 &&
		(sio->status & PSX_SIO_STATUS_TX_EMPTY) == 0) {
		sio->tx_bits = 8;
		sio->tx_shift = sio->tx_data;
		if (sio->variant == PSX_SIO0) {
			sio->rx_bits = 8;
			sio->rx_shift = 0;
		}
		sio->status |= PSX_SIO_STATUS_TX_EMPTY | PSX_SIO_STATUS_TX_RDY;
	}

	if (sio->tx_bits != 0) {
		if (sio->variant == PSX_SIO0)
			call_line(sio->callbacks.sck, sio, 0);
		call_line(sio->callbacks.txd, sio, (int)(sio->tx_shift & 1u));
		sio->tx_shift >>= 1;
		sio->tx_bits--;
		if (sio->variant == PSX_SIO0)
			call_line(sio->callbacks.sck, sio, 1);
		if (sio->tx_bits == 0 &&
			(sio->control & PSX_SIO_CONTROL_TX_IENA) != 0)
			sio_interrupt(sio);
	}

	if (sio->rx_bits != 0) {
		sio->rx_shift = (sio->rx_shift >> 1) | ((uint32_t)sio->rxd << 7);
		sio->rx_bits--;
		if (sio->rx_bits == 0) {
			if ((sio->status & PSX_SIO_STATUS_RX_RDY) != 0) {
				sio->status |= PSX_SIO_STATUS_OVERRUN;
			} else {
				sio->rx_data = sio->rx_shift;
				sio->status |= PSX_SIO_STATUS_RX_RDY;
			}
			if ((sio->control & PSX_SIO_CONTROL_RX_IENA) != 0)
				sio_interrupt(sio);
		}
	}

	result = sio_timer_adjust(sio);
	return result;
}

psx_sio_result_t psx_sio_create(psx_sio_variant_t variant,
		const psx_sio_callbacks_t *callbacks, psx_sio_t **out_sio)
{
	psx_sio_t *sio;
	if (out_sio != NULL)
		*out_sio = NULL;
	if (out_sio == NULL || (variant != PSX_SIO0 && variant != PSX_SIO1))
		return PSX_SIO_ARGUMENT;
	sio = (psx_sio_t *)calloc(1, sizeof(*sio));
	if (sio == NULL)
		return PSX_SIO_NOMEM;
	sio->variant = variant;
	if (callbacks != NULL)
		sio->callbacks = *callbacks;
	sio->status = PSX_SIO_STATUS_TX_EMPTY | PSX_SIO_STATUS_TX_RDY;
	sio->rxd = 1;
	*out_sio = sio;
	return PSX_SIO_OK;
}

void psx_sio_destroy(psx_sio_t *sio)
{
	free(sio);
}

psx_sio_result_t psx_sio_read(psx_sio_t *sio, uint32_t offset,
		uint32_t mem_mask, uint32_t *value)
{
	uint32_t data;
	if (sio == NULL || value == NULL)
		return PSX_SIO_ARGUMENT;
	switch (offset % 4u) {
	case 0:
		data = sio->rx_data;
		sio->status &= ~PSX_SIO_STATUS_RX_RDY;
		sio->rx_data = 0xff;
		break;
	case 1:
		data = sio->status;
		break;
	case 2:
		data = (sio->control << 16) | (sio->mode & 0xffffu);
		break;
	case 3:
		data = sio->baud << 16;
		break;
	default:
		data = 0;
		break;
	}
	(void)mem_mask; /* MAME logs lanes on reads but returns the whole word. */
	*value = data;
	return PSX_SIO_OK;
}

psx_sio_result_t psx_sio_write(psx_sio_t *sio, uint32_t offset,
		uint32_t value, uint32_t mem_mask)
{
	psx_sio_result_t result = PSX_SIO_OK;
	if (sio == NULL)
		return PSX_SIO_ARGUMENT;
	switch (offset % 4u) {
	case 0:
		sio->tx_data = value;
		sio->status &= ~(PSX_SIO_STATUS_TX_RDY | PSX_SIO_STATUS_TX_EMPTY);
		result = sio_timer_adjust(sio);
		break;
	case 1:
		/* The upstream status register ignores writes. */
		break;
	case 2:
		if (mem_mask & 0x0000ffffu)
			sio->mode = value & 0xffffu;
		if (mem_mask & 0xffff0000u) {
			sio->control = value >> 16;
			if (sio->control & PSX_SIO_CONTROL_RESET) {
				sio->status |= PSX_SIO_STATUS_TX_EMPTY | PSX_SIO_STATUS_TX_RDY;
				sio->status &= ~(PSX_SIO_STATUS_RX_RDY | PSX_SIO_STATUS_OVERRUN |
					PSX_SIO_STATUS_IRQ);
				call_line(sio->callbacks.irq, sio, 0);
				call_line(sio->callbacks.dtr, sio, 0);
				call_line(sio->callbacks.dtr, sio, 1);
				sio->tx_bits = 0;
				sio->rx_bits = 0;
				call_line(sio->callbacks.txd, sio, 1);
			}
			if (sio->control & PSX_SIO_CONTROL_IACK) {
				sio->status &= ~PSX_SIO_STATUS_IRQ;
				sio->control &= ~PSX_SIO_CONTROL_IACK;
				call_line(sio->callbacks.irq, sio, 0);
			}
			call_line(sio->callbacks.dtr, sio,
				(sio->control & PSX_SIO_CONTROL_DTR) ? 0 : 1);
		}
		break;
	case 3:
		if (mem_mask & 0x0000ffffu) {
			/* MAME's log-only low half does not change baud. */
		}
		if (mem_mask & 0xffff0000u)
			sio->baud = value >> 16;
		break;
	default:
		break;
	}
	return result;
}

void psx_sio_write_rxd(psx_sio_t *sio, int state)
{
	if (sio != NULL)
		sio->rxd = state;
}

void psx_sio_write_dsr(psx_sio_t *sio, int state)
{
	if (sio == NULL)
		return;
	if (state) {
		sio->status &= ~PSX_SIO_STATUS_DSR;
	} else if ((sio->status & PSX_SIO_STATUS_DSR) == 0) {
		sio->status |= PSX_SIO_STATUS_DSR;
		if (sio->control & PSX_SIO_CONTROL_DSR_IENA)
			sio_interrupt(sio);
	}
}

psx_sio_result_t psx_sio_advance_to_cycle(psx_sio_t *sio, uint64_t cycle)
{
	if (sio == NULL)
		return PSX_SIO_ARGUMENT;
	if (cycle < sio->now_cycle)
		return PSX_SIO_TIME_REWIND;
	while (sio->timer_pending && sio->deadline <= cycle) {
		psx_sio_result_t result;
		sio->now_cycle = sio->deadline;
		sio->timer_pending = 0;
		result = sio_tick(sio);
		if (result != PSX_SIO_OK)
			return result;
	}
	sio->now_cycle = cycle;
	return PSX_SIO_OK;
}

uint64_t psx_sio_elapsed_cycles(const psx_sio_t *sio)
{
	return sio != NULL ? sio->now_cycle : 0;
}
