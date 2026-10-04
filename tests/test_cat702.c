/* CAT702 pin-level tests against an offline scalar reference of MAME's equations. */
#include "../libpcsxcore/arcade/cat702.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct output_log {
	unsigned count;
	int last;
} output_log_t;

static void output_changed(void *opaque, int state)
{
	output_log_t *log = (output_log_t *)opaque;
	log->count++;
	log->last = state;
}

static void test_synthetic_key_vector_and_edges(void)
{
	/* Synthetic ROM key, and scalar-reference response for the four input bytes. */
	static const uint8_t key[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
	static const uint8_t input[4] = { 0x00, 0x01, 0xa5, 0xff };
	static const uint8_t expected[4] = { 0xb5, 0xdb, 0x94, 0x6a };
	cat702_t device;
	output_log_t log = { 0, 0 };
	unsigned i;

	cat702_init(&device, key, sizeof(key), output_changed, &log);
	assert(cat702_dataout(&device) == 1);
	assert(log.count == 1 && log.last == 1);

	/* Clocks while deselected have no effect. */
	cat702_write_clock(&device, 0);
	cat702_write_clock(&device, 1);
	assert(log.count == 1);

	cat702_write_select(&device, 0);
	for (i = 0; i < 4; i++)
		assert(cat702_exchange_byte_lsb_first(&device, input[i]) == expected[i]);
	assert(log.count == 33); /* One init level plus one output callback per clock. */

	/* Deselect returns DATA OUT high; later clocks remain ignored. */
	cat702_write_select(&device, 1);
	assert(cat702_dataout(&device) == 1);
	assert(log.count == 34 && log.last == 1);
	cat702_write_clock(&device, 0);
	cat702_write_clock(&device, 1);
	assert(log.count == 34);
}

static void test_default_key_and_reselection(void)
{
	static const uint8_t input[] = { 0x00, 0x01, 0xa5, 0xff };
	static const uint8_t expected_default[] = { 0xff, 0xff, 0xff, 0xbf };
	cat702_t device;
	unsigned i;
	cat702_init(&device, NULL, 0, NULL, NULL);
	for (i = 0; i < sizeof(device.transform); i++)
		assert(device.transform[i] == 0xff);

	cat702_write_select(&device, 0);
	for (i = 0; i < sizeof(input); i++)
		assert(cat702_exchange_byte_lsb_first(&device, input[i]) == expected_default[i]);
	cat702_write_select(&device, 1);

	/* A fresh selection restarts from state 0xfc and transform byte zero. */
	cat702_write_select(&device, 0);
	assert(device.state == 0xfc);
	assert(device.bit == 0);
	cat702_write_clock(&device, 0);
	assert(device.bit == 0);
}

int main(void)
{
	test_synthetic_key_vector_and_edges();
	test_default_key_and_reselection();
	puts("CAT702 protocol tests passed");
	return 0;
}
