/*
 * Standalone Namco settings serial sink and Epson RTC-4543.
 *
 * Protocol behavior follows MAME at deb2f54fe7300e7a192690318beb860159bfd52a:
 * src/mame/namco/namco_settings.cpp and
 * src/devices/machine/rtc4543.cpp.  The Namco device is a write-only frame
 * receiver in that implementation; a completed frame is exposed as telemetry
 * and is not treated as an EEPROM/register array.
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LIBPCSXCORE_ARCADE_SERIAL_NVRAM_H
#define LIBPCSXCORE_ARCADE_SERIAL_NVRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*namco_settings_frame_cb)(void *opaque, uint8_t address, uint8_t value);

typedef struct namco_settings_serial {
	int ce;
	int clk;
	int data;
	unsigned bit_count;
	uint8_t address;
	uint8_t value;
	uint8_t last_address;
	uint8_t last_value;
	uint64_t frames_received;
	namco_settings_frame_cb frame_cb;
	void *frame_opaque;
} namco_settings_serial_t;

void namco_settings_init(namco_settings_serial_t *device,
		namco_settings_frame_cb callback, void *opaque);
void namco_settings_reset(namco_settings_serial_t *device);
void namco_settings_ce_w(namco_settings_serial_t *device, int state);
void namco_settings_clk_w(namco_settings_serial_t *device, int state);
void namco_settings_data_w(namco_settings_serial_t *device, int state);
/* MAME's settings sink has no DATA output pin; this returns its idle low level. */
int namco_settings_data_r(const namco_settings_serial_t *device);
/* Generic fixture helper; the byte is shifted MSB first. H8 SCI uses LSB first. */
void namco_settings_write_byte_msb_first(namco_settings_serial_t *device, uint8_t value);
/* H8 SCI wire-order helper for callers that do not model TX/CLK separately. */
void namco_settings_write_byte_lsb_first(namco_settings_serial_t *device, uint8_t value);

typedef void (*rtc4543_data_cb)(void *opaque, int bit);

typedef struct rtc4543_datetime {
	int year;       /* Full year accepted by set_time; wire register stores 00-99. */
	int month;      /* 1-12 */
	int day;        /* 1-31 */
	int day_of_week;/* ISO weekday: Monday=1 ... Sunday=7; 0 asks set_time to derive it. */
	int hour;       /* 0-23 */
	int minute;     /* 0-59 */
	int second;     /* 0-59 */
} rtc4543_datetime_t;

typedef struct rtc4543_serial {
	int ce;
	int clk;
	int wr;
	int data;
	unsigned bit_count; /* 0..56; bits 28..31 are skipped on the wire. */
	uint8_t regs[7];    /* BCD: seconds, minutes, hours, weekday, day, month, year. */
	rtc4543_data_cb data_cb;
	void *data_opaque;
} rtc4543_serial_t;

void rtc4543_init(rtc4543_serial_t *device, rtc4543_data_cb callback,
		void *opaque, const rtc4543_datetime_t *initial_time);
void rtc4543_reset(rtc4543_serial_t *device);
void rtc4543_set_time(rtc4543_serial_t *device, const rtc4543_datetime_t *time);
void rtc4543_get_time(const rtc4543_serial_t *device, rtc4543_datetime_t *time);
/* Advances a deterministic emulated clock; it never consults wall time. */
void rtc4543_advance_seconds(rtc4543_serial_t *device, uint64_t seconds);
void rtc4543_ce_w(rtc4543_serial_t *device, int state);
void rtc4543_wr_w(rtc4543_serial_t *device, int state);
void rtc4543_clk_w(rtc4543_serial_t *device, int state);
void rtc4543_data_w(rtc4543_serial_t *device, int state);
int rtc4543_data_r(const rtc4543_serial_t *device);
/* Helper for byte-oriented SCI adapters; sends bits LSB first as RTC data does. */
void rtc4543_write_byte_lsb_first(rtc4543_serial_t *device, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
