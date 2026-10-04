/* Protocol tests for the standalone MAME-derived Namco serial devices. */
#include "../libpcsxcore/arcade/serial_nvram.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct frame_observer {
	unsigned calls;
	uint8_t address;
	uint8_t value;
} frame_observer_t;

typedef struct bit_observer {
	unsigned calls;
	int bits[64];
} bit_observer_t;

static void settings_frame(void *opaque, uint8_t address, uint8_t value)
{
	frame_observer_t *observer = (frame_observer_t *)opaque;
	observer->calls++;
	observer->address = address;
	observer->value = value;
}

static void rtc_bit(void *opaque, int bit)
{
	bit_observer_t *observer = (bit_observer_t *)opaque;
	assert(observer->calls < sizeof(observer->bits) / sizeof(observer->bits[0]));
	observer->bits[observer->calls++] = bit;
}

static uint8_t reverse_bits(uint8_t value)
{
	uint8_t reversed = 0;
	unsigned bit;
	for (bit = 0; bit < 8; bit++)
		reversed = (uint8_t)((reversed << 1) | ((value >> bit) & 1));
	return reversed;
}

static void test_settings_frame_protocol(void)
{
	namco_settings_serial_t device;
	frame_observer_t observer = { 0, 0, 0 };
	namco_settings_init(&device, settings_frame, &observer);

	/* The settings endpoint ignores clocks while CE is low. */
	namco_settings_write_byte_msb_first(&device, 0x96);
	assert(device.bit_count == 0);
	assert(observer.calls == 0);
	assert(namco_settings_data_r(&device) == 0);

	namco_settings_ce_w(&device, 1);
	namco_settings_write_byte_msb_first(&device, 0x96);
	namco_settings_write_byte_msb_first(&device, 0x3a);
	assert(observer.calls == 1);
	assert(observer.address == reverse_bits(0x96));
	assert(observer.value == 0x3a);
	assert(device.last_address == reverse_bits(0x96));
	assert(device.last_value == 0x3a);
	assert(device.frames_received == 1);
	assert(device.bit_count == 0);

	/* A CE rising edge resets only the protocol bit index, as in MAME. */
	namco_settings_write_byte_msb_first(&device, 0x12);
	assert(device.bit_count == 8);
	namco_settings_ce_w(&device, 0);
	namco_settings_ce_w(&device, 1);
	assert(device.bit_count == 0);
	namco_settings_reset(&device);
	namco_settings_ce_w(&device, 1);
	namco_settings_write_byte_lsb_first(&device, 0x56);
	namco_settings_write_byte_lsb_first(&device, 0x3a);
	assert(observer.calls == 2);
	assert(observer.address == 0x56);
	assert(observer.value == reverse_bits(0x3a));
	namco_settings_reset(&device);
	assert(device.frames_received == 0);
	assert(device.frame_cb == settings_frame);
}

static unsigned rtc_wire_position(unsigned logical_bit)
{
	return logical_bit < 28 ? logical_bit : logical_bit + 4;
}

static void test_rtc4543_read_order_and_gap(void)
{
	rtc4543_serial_t rtc;
	rtc4543_datetime_t date = { 2024, 2, 28, 3, 23, 59, 58 };
	bit_observer_t observer;
	unsigned logical;
	memset(&observer, 0, sizeof(observer));
	rtc4543_init(&rtc, rtc_bit, &observer, &date);
	rtc4543_ce_w(&rtc, 1);

	for (logical = 0; logical < 52; logical++) {
		unsigned wire = rtc_wire_position(logical);
		unsigned reg = wire / 8;
		unsigned bit = wire & 7;
		int expected = (rtc.regs[reg] >> bit) & 1;
		rtc4543_clk_w(&rtc, 1);
		assert(rtc4543_data_r(&rtc) == expected);
		rtc4543_clk_w(&rtc, 0);
	}
	assert(observer.calls == 52);
	assert(observer.bits[0] == ((0x58 >> 0) & 1));
	assert(observer.bits[7] == ((0x58 >> 7) & 1));
	assert(observer.bits[24] == ((0x03 >> 0) & 1));
	assert(observer.bits[27] == ((0x03 >> 3) & 1));
	assert(observer.bits[28] == ((0x28 >> 0) & 1)); /* DOW upper nibble is skipped. */
	assert(observer.bits[51] == ((0x24 >> 7) & 1));
	assert(rtc.bit_count == 56);
}

static void test_rtc_deterministic_time_advance(void)
{
	rtc4543_serial_t rtc;
	rtc4543_datetime_t date = { 2024, 2, 28, 3, 23, 59, 59 };
	rtc4543_datetime_t now;
	rtc4543_init(&rtc, NULL, NULL, &date);
	rtc4543_advance_seconds(&rtc, 1);
	rtc4543_get_time(&rtc, &now);
	assert(now.year == 2024);
	assert(now.month == 2);
	assert(now.day == 29);
	assert(now.day_of_week == 4);
	assert(now.hour == 0 && now.minute == 0 && now.second == 0);

	rtc4543_advance_seconds(&rtc, 86400);
	rtc4543_get_time(&rtc, &now);
	assert(now.day == 1 && now.month == 3 && now.day_of_week == 5);
}

static void test_rtc_full_write_and_data_callback_quiet(void)
{
	rtc4543_serial_t rtc;
	rtc4543_datetime_t date = { 2024, 1, 1, 0, 0, 0, 0 };
	rtc4543_datetime_t now;
	bit_observer_t observer;
	const uint8_t payload[7] = { 0x56, 0x34, 0x12, 0x04, 0x29, 0x02, 0x24 };
	unsigned reg;
	unsigned bit;
	memset(&observer, 0, sizeof(observer));
	rtc4543_init(&rtc, rtc_bit, &observer, &date);
	rtc4543_ce_w(&rtc, 1);
	rtc4543_wr_w(&rtc, 1);
	for (reg = 0; reg < 7; reg++) {
		unsigned bit_count = reg == 3 ? 4 : 8;
		for (bit = 0; bit < bit_count; bit++) {
			rtc4543_data_w(&rtc, (payload[reg] >> bit) & 1);
			rtc4543_clk_w(&rtc, 1);
			rtc4543_clk_w(&rtc, 0);
		}
	}
	assert(rtc.bit_count == 56);
	assert(observer.calls == 0);
	rtc4543_get_time(&rtc, &now);
	assert(now.year == 2024 && now.month == 2 && now.day == 29);
	assert(now.day_of_week == 4);
	assert(now.hour == 12 && now.minute == 34 && now.second == 56);
}

int main(void)
{
	test_settings_frame_protocol();
	test_rtc4543_read_order_and_gap();
	test_rtc_deterministic_time_advance();
	test_rtc_full_write_and_data_callback_quiet();
	puts("serial NVRAM protocol tests passed");
	return 0;
}
