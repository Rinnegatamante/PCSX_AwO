/*
 * Namco settings serial sink and Epson RTC-4543 protocol model.
 * Derived from MAME:
 *   src/mame/namco/namco_settings.cpp (Olivier Galibert)
 *   src/devices/machine/rtc4543.cpp (R. Belmont)
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 *
 * license:BSD-3-Clause
 * copyright-holders:Olivier Galibert, R. Belmont
 * Copyright (C) 2026
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "serial_nvram.h"

#include <string.h>

static uint8_t to_bcd(int value)
{
	return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static int from_bcd(uint8_t value)
{
	return (((value & 0xf0) >> 4) * 10) + (value & 0x0f);
}

static int leap_year(int year)
{
	/* The chip exposes two year digits and MAME's RTC interface uses % 4. */
	return year % 4 == 0;
}

static int days_in_month(int year, int month)
{
	static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (month < 1 || month > 12)
		return 31;
	if (month == 2 && leap_year(year))
		return 29;
	return days[month - 1];
}

static int iso_weekday(int year, int month, int day)
{
	/* Gregorian calendar; result is Monday=1 through Sunday=7. */
	static const int offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	int full_year = year;
	int sunday_zero;
	if (month < 3)
		full_year--;
	sunday_zero = (full_year + full_year / 4 - full_year / 100 + full_year / 400
			+ offsets[month - 1] + day) % 7;
	return sunday_zero == 0 ? 7 : sunday_zero;
}

static void update_registers_from_time(rtc4543_serial_t *device,
		const rtc4543_datetime_t *time)
{
	device->regs[0] = to_bcd(time->second);
	device->regs[1] = to_bcd(time->minute);
	device->regs[2] = to_bcd(time->hour);
	device->regs[3] = to_bcd(time->day_of_week);
	device->regs[4] = to_bcd(time->day);
	device->regs[5] = to_bcd(time->month);
	device->regs[6] = to_bcd(time->year % 100);
}

void namco_settings_init(namco_settings_serial_t *device,
		namco_settings_frame_cb callback, void *opaque)
{
	memset(device, 0, sizeof(*device));
	device->frame_cb = callback;
	device->frame_opaque = opaque;
	device->clk = 1;
}

void namco_settings_reset(namco_settings_serial_t *device)
{
	namco_settings_frame_cb callback = device->frame_cb;
	void *opaque = device->frame_opaque;
	memset(device, 0, sizeof(*device));
	device->frame_cb = callback;
	device->frame_opaque = opaque;
	device->clk = 1;
}

void namco_settings_ce_w(namco_settings_serial_t *device, int state)
{
	state = !!state;
	if (device->ce != state) {
		device->ce = state;
		if (state)
			device->bit_count = 0;
	}
}

void namco_settings_clk_w(namco_settings_serial_t *device, int state)
{
	state = !!state;
	if (device->clk != state) {
		device->clk = state;
		if (state && device->ce) {
			if (device->bit_count < 8)
				device->address = (uint8_t)((device->address >> 1) | (device->data << 7));
			else
				device->value = (uint8_t)((device->value << 1) | device->data);
			device->bit_count++;
			if (device->bit_count == 16) {
				device->last_address = device->address;
				device->last_value = device->value;
				device->frames_received++;
				device->bit_count = 0;
				if (device->frame_cb)
					device->frame_cb(device->frame_opaque,
							device->last_address, device->last_value);
			}
		}
	}
}

void namco_settings_data_w(namco_settings_serial_t *device, int state)
{
	device->data = !!state;
}

int namco_settings_data_r(const namco_settings_serial_t *device)
{
	(void)device;
	return 0;
}

void namco_settings_write_byte_msb_first(namco_settings_serial_t *device, uint8_t value)
{
	int bit;
	for (bit = 7; bit >= 0; bit--) {
		namco_settings_data_w(device, (value >> bit) & 1);
		namco_settings_clk_w(device, 0);
		namco_settings_clk_w(device, 1);
	}
}

void namco_settings_write_byte_lsb_first(namco_settings_serial_t *device, uint8_t value)
{
	unsigned bit;
	for (bit = 0; bit < 8; bit++) {
		namco_settings_data_w(device, (value >> bit) & 1);
		namco_settings_clk_w(device, 0);
		namco_settings_clk_w(device, 1);
	}
}

void rtc4543_init(rtc4543_serial_t *device, rtc4543_data_cb callback,
		void *opaque, const rtc4543_datetime_t *initial_time)
{
	memset(device, 0, sizeof(*device));
	device->data_cb = callback;
	device->data_opaque = opaque;
	if (initial_time)
		rtc4543_set_time(device, initial_time);
}

void rtc4543_reset(rtc4543_serial_t *device)
{
	rtc4543_data_cb callback = device->data_cb;
	void *opaque = device->data_opaque;
	uint8_t registers[7];
	memcpy(registers, device->regs, sizeof(registers));
	memset(device, 0, sizeof(*device));
	memcpy(device->regs, registers, sizeof(registers));
	device->data_cb = callback;
	device->data_opaque = opaque;
}

void rtc4543_set_time(rtc4543_serial_t *device, const rtc4543_datetime_t *time)
{
	rtc4543_datetime_t normalized = *time;
	normalized.year %= 100;
	if (normalized.year < 0)
		normalized.year += 100;
	if (normalized.day_of_week < 1 || normalized.day_of_week > 7)
		normalized.day_of_week = iso_weekday(normalized.year + 2000,
				normalized.month, normalized.day);
	update_registers_from_time(device, &normalized);
}

void rtc4543_get_time(const rtc4543_serial_t *device, rtc4543_datetime_t *time)
{
	time->second = from_bcd(device->regs[0]);
	time->minute = from_bcd(device->regs[1]);
	time->hour = from_bcd(device->regs[2]);
	time->day_of_week = from_bcd(device->regs[3]);
	time->day = from_bcd(device->regs[4]);
	time->month = from_bcd(device->regs[5]);
	time->year = 2000 + from_bcd(device->regs[6]);
}

void rtc4543_advance_seconds(rtc4543_serial_t *device, uint64_t seconds)
{
	rtc4543_datetime_t now;
	uint64_t day_seconds;
	uint64_t days;
	rtc4543_get_time(device, &now);

	day_seconds = (uint64_t)now.hour * 3600 + (uint64_t)now.minute * 60 + (uint64_t)now.second;
	day_seconds += seconds % 86400;
	days = seconds / 86400 + day_seconds / 86400;
	day_seconds %= 86400;
	now.hour = (int)(day_seconds / 3600);
	now.minute = (int)((day_seconds / 60) % 60);
	now.second = (int)(day_seconds % 60);

	while (days) {
		unsigned remaining = (unsigned)(days_in_month(now.year % 100, now.month) - now.day + 1);
		if (days < remaining) {
			now.day += (int)days;
			now.day_of_week = (now.day_of_week - 1 + (int)(days % 7)) % 7 + 1;
			days = 0;
		} else {
			days -= remaining;
			now.month++;
			now.day = 1;
			now.day_of_week = now.day_of_week % 7 + 1;
			if (now.month > 12) {
				now.month = 1;
				now.year++;
				if (now.year > 2099)
					now.year = 2000;
			}
		}
	}
	update_registers_from_time(device, &now);
}

void rtc4543_ce_w(rtc4543_serial_t *device, int state)
{
	state = !!state;
	if (state && !device->ce)
		device->bit_count = 0;
	device->ce = state;
}

void rtc4543_wr_w(rtc4543_serial_t *device, int state)
{
	device->wr = !!state;
}

static void rtc4543_advance_bit(rtc4543_serial_t *device)
{
	device->bit_count++;
	if (device->bit_count == 28)
		device->bit_count += 4;
}

static void rtc4543_load_bit(rtc4543_serial_t *device, unsigned reg)
{
	unsigned bit = device->bit_count & 7;
	device->data = (device->regs[reg] >> bit) & 1;
	if (device->data_cb)
		device->data_cb(device->data_opaque, device->data);
}

static void rtc4543_store_bit(rtc4543_serial_t *device, unsigned reg)
{
	unsigned bit = device->bit_count & 7;
	device->regs[reg] = (uint8_t)((device->regs[reg] & ~(1u << bit))
			| ((unsigned)device->data << bit));
}

static void rtc4543_update_time_from_registers(rtc4543_serial_t *device)
{
	/*
	 * The interface's set_time(false, ...) stores the decoded values in its
	 * clock registers but does not call rtc_clock_updated(), so the serial
	 * register bytes retain the exact values written by software.
	 */
	(void)device;
}

void rtc4543_clk_w(rtc4543_serial_t *device, int state)
{
	int rising;
	state = !!state;
	rising = !device->clk && state;
	if (device->ce && rising && device->bit_count < 56) {
		unsigned reg = device->bit_count / 8;
		if (device->wr)
			rtc4543_store_bit(device, reg);
		else
			rtc4543_load_bit(device, reg);
		rtc4543_advance_bit(device);
		if (device->wr && device->bit_count == 56)
			rtc4543_update_time_from_registers(device);
	}
	device->clk = state;
}

void rtc4543_data_w(rtc4543_serial_t *device, int state)
{
	device->data = !!state;
}

int rtc4543_data_r(const rtc4543_serial_t *device)
{
	return device->data;
}

void rtc4543_write_byte_lsb_first(rtc4543_serial_t *device, uint8_t value)
{
	unsigned bit;
	for (bit = 0; bit < 8; bit++) {
		rtc4543_data_w(device, (value >> bit) & 1);
		rtc4543_clk_w(device, 0);
		rtc4543_clk_w(device, 1);
	}
}
