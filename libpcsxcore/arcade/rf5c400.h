/* SPDX-License-Identifier: BSD-3-Clause
 * MAME Ricoh RF5C400, copyright holder Ville Linde. See rf5c400_LICENSE.txt.
 */
#ifndef PCSX_ARCADE_RF5C400_H
#define PCSX_ARCADE_RF5C400_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RF5C400_CHANNELS 32u
#define RF5C400_OUTPUTS 4u
#define RF5C400_DEFAULT_CLOCK 16934400u
#define RF5C400_ADDRESS_MASK 0x01ffffffu

typedef struct RF5C400 RF5C400;
enum { RF5C400_OK=0, RF5C400_ARGUMENT=-1, RF5C400_NOMEM=-2,
       RF5C400_REENTRANT=-3, RF5C400_TIME_REWIND=-4, RF5C400_OVERFLOW=-5 };
typedef struct RF5C400Config {
    uint32_t clock_hz; /* zero selects Twinkle's 16.9344 MHz */
    uint64_t cycle_origin; /* creation epoch; first sample is one period later */
    const uint8_t *sample_rom; /* borrowed little-endian bytes, optional with read_word */
    size_t sample_rom_size;
    /* Aligned 25-bit BYTE addresses, not word offsets. Callbacks may query
     * status but must not mutate, render, advance or destroy this device. */
    uint16_t (*read_word)(void *opaque, uint32_t address);
    void (*write_word)(void *opaque, uint32_t address, uint16_t value);
    void *memory_opaque;
    /* Unclipped sums in signed PCM units (32768 = full scale), output order
     * direct L/R, external-effect L/R. No internal effect processor invented.
     * cycle is the exact completed-frame chip cycle on clocked runs. */
    void (*sample)(void *opaque, const int32_t outputs[4], uint64_t cycle);
    void *sample_opaque;
} RF5C400Config;
typedef struct RF5C400Channel {

		uint16_t startH;
		uint16_t startL;
		uint16_t freq;
		uint16_t endL;
		uint16_t endHloopH;
		uint16_t loopL;
		uint16_t pan;
		uint16_t effect;
		uint16_t volume;

		uint16_t attack;
		uint16_t decay;
		uint16_t release;

		uint16_t cutoff;

		uint64_t pos;
		uint64_t step;
		uint16_t keyon;

		uint8_t env_phase;
		double env_level;
		double env_step;
		double env_scale;

} RF5C400Channel;
typedef struct RF5C400Status {
    uint32_t clock_hz, sample_rate, external_address;
    uint16_t status, external_data, requested_channel;
    uint64_t cycles, clock_frames, rendered_frames, sample_phase;
} RF5C400Status;
int rf5c400_create(const RF5C400Config *config, RF5C400 **out);
void rf5c400_destroy(RF5C400 *device);
/* Upstream has no device_reset override: soft reset preserves chip state. */
int rf5c400_reset(RF5C400 *device);
/* WORD offsets 000..7ff. Upstream ignores mem_mask even on partial writes;
 * board adapters must preserve this source-visible behavior. Advance to the
 * access cycle before using these immediate register operations. */
int rf5c400_read(RF5C400 *device, uint32_t offset, uint16_t mask, uint16_t *value);
int rf5c400_write(RF5C400 *device, uint32_t offset, uint16_t value, uint16_t mask);
int rf5c400_run_cycles(RF5C400 *device, uint64_t cycles);
int rf5c400_advance_to_cycle(RF5C400 *device, uint64_t cycle);
/* Offline stream update; does not advance the clock or invoke sample callback.
 * Optional interleaved four-output buffer, capacity in frames. */
int rf5c400_render(RF5C400 *device, size_t frames, int32_t *outputs, size_t capacity);
int rf5c400_get_channel(const RF5C400 *device, unsigned channel, RF5C400Channel *state);
void rf5c400_get_status(const RF5C400 *device, RF5C400Status *status);
#ifdef __cplusplus
}
#endif
#endif
