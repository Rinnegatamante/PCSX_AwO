/*
 * CAT702 serial security device.
 * Derived from MAME src/devices/machine/cat702.cpp (smf).
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 *
 * license:BSD-3-Clause
 * copyright-holders:smf
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "cat702.h"

#include <string.h>

static const uint8_t initial_sbox[8] = {
	0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x7f
};

static unsigned bit_at(uint8_t value, unsigned bit)
{
	return (value >> bit) & 1u;
}

static uint8_t compute_sbox_coef(const cat702_t *device, unsigned sel, unsigned bit)
{
	uint8_t value;
	if (!sel)
		return device->transform[bit];

	value = compute_sbox_coef(device, (sel - 1) & 7, (bit - 1) & 7);
	value = (uint8_t)((value << 1) | (bit_at(value, 7) ^ bit_at(value, 6)));
	if (bit != 7)
		return value;
	return (uint8_t)(value ^ compute_sbox_coef(device, sel, 0));
}

static void apply_sbox(cat702_t *device, const uint8_t coefficients[8])
{
	uint8_t result = 0;
	unsigned bit;
	for (bit = 0; bit < 8; bit++) {
		if (bit_at(device->state, bit))
			result ^= coefficients[bit];
	}
	device->state = result;
}

static void apply_bit_sbox(cat702_t *device, unsigned select)
{
	uint8_t coefficients[8];
	unsigned bit;
	uint8_t result = 0;
	for (bit = 0; bit < 8; bit++)
		coefficients[bit] = compute_sbox_coef(device, select, bit);
	for (bit = 0; bit < 8; bit++) {
		if (bit_at(device->state, bit))
			result ^= coefficients[bit];
	}
	device->state = result;
}

static void set_dataout(cat702_t *device, int state)
{
	device->dataout = !!state;
	if (device->dataout_cb)
		device->dataout_cb(device->dataout_opaque, device->dataout);
}

void cat702_init(cat702_t *device, const uint8_t *transform, size_t length,
		cat702_dataout_cb callback, void *opaque)
{
	memset(device, 0, sizeof(*device));
	if (transform && length == sizeof(device->transform))
		memcpy(device->transform, transform, sizeof(device->transform));
	else
		memset(device->transform, 0xff, sizeof(device->transform));
	device->select = 1;
	device->clock = 1;
	device->datain = 1;
	device->dataout = 1;
	device->dataout_cb = callback;
	device->dataout_opaque = opaque;
	if (callback)
		callback(opaque, 1);
}

void cat702_reset(cat702_t *device)
{
	uint8_t transform[8];
	cat702_dataout_cb callback = device->dataout_cb;
	void *opaque = device->dataout_opaque;
	memcpy(transform, device->transform, sizeof(transform));
	cat702_init(device, transform, sizeof(transform), callback, opaque);
}

void cat702_write_select(cat702_t *device, int state)
{
	state = !!state;
	if (device->select != state) {
		if (!state) {
			device->state = 0xfc;
			device->bit = 0;
		} else {
			set_dataout(device, 1);
		}
		device->select = state;
	}
}

void cat702_write_clock(cat702_t *device, int state)
{
	state = !!state;
	if (!state && device->clock && !device->select) {
		if (device->bit == 0)
			apply_sbox(device, initial_sbox);
		set_dataout(device, (int)bit_at(device->state, device->bit));
	}

	if (state && !device->clock && !device->select) {
		if (!device->datain)
			apply_bit_sbox(device, device->bit);
		device->bit = (uint8_t)((device->bit + 1) & 7);
	}
	device->clock = state;
}

void cat702_write_datain(cat702_t *device, int state)
{
	device->datain = !!state;
}

int cat702_dataout(const cat702_t *device)
{
	return device->dataout;
}

uint8_t cat702_exchange_byte_lsb_first(cat702_t *device, uint8_t input)
{
	uint8_t output = 0;
	unsigned bit;
	for (bit = 0; bit < 8; bit++) {
		cat702_write_datain(device, (input >> bit) & 1);
		cat702_write_clock(device, 0);
		output |= (uint8_t)(cat702_dataout(device) << bit);
		cat702_write_clock(device, 1);
	}
	return output;
}
