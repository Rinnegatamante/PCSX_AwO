/*
 * Standalone CAT702 serial security device.
 * Protocol and transform follow MAME src/devices/machine/cat702.cpp
 * (copyright-holder smf), pinned at
 * deb2f54fe7300e7a192690318beb860159bfd52a.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LIBPCSXCORE_ARCADE_CAT702_H
#define LIBPCSXCORE_ARCADE_CAT702_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cat702_dataout_cb)(void *opaque, int state);

typedef struct cat702 {
	int select; /* Active low. */
	int clock;
	int datain;
	int dataout;
	uint8_t state;
	uint8_t bit;
	uint8_t transform[8];
	cat702_dataout_cb dataout_cb;
	void *dataout_opaque;
} cat702_t;

/*
 * Copies exactly eight transform bytes. A missing or incorrectly sized key
 * region matches MAME by using eight 0xff bytes.
 */
void cat702_init(cat702_t *device, const uint8_t *transform, size_t length,
		cat702_dataout_cb callback, void *opaque);
void cat702_reset(cat702_t *device);
void cat702_write_select(cat702_t *device, int state);
void cat702_write_clock(cat702_t *device, int state);
void cat702_write_datain(cat702_t *device, int state);
int cat702_dataout(const cat702_t *device);

/* Convenience adapter for hosts that already have one LSB-first serial byte. */
uint8_t cat702_exchange_byte_lsb_first(cat702_t *device, uint8_t input);

#ifdef __cplusplus
}
#endif

#endif
