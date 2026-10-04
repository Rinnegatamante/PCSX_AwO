/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright holders: Olivier Galibert, R. Belmont, hap, superctr.
 * See zsg2.LICENSE. Standalone adaptation of pinned MAME ZSG-2. */
#ifndef PCSX_ARCADE_ZSG2_H
#define PCSX_ARCADE_ZSG2_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ZSG2_CHANNELS 48u
#define ZSG2_OUTPUTS 4u
#define ZSG2_DEFAULT_CLOCK 25000000u
#define ZSG2_PERIODIC_UPDATE_HZ 50u
#define ZSG2_ACTIVE 0x8000u

typedef struct ZSG2 ZSG2;
enum {
    ZSG2_OK = 0, ZSG2_ARGUMENT = -1, ZSG2_NOMEM = -2,
    ZSG2_MASK_UNSUPPORTED = -3, ZSG2_TIME_REWIND = -4,
    ZSG2_TIME_OVERFLOW = -5, ZSG2_CAPACITY = -6, ZSG2_REENTRANT = -7
};
enum {
    ZSG2_DIAG_PARTIAL_ACCESS = 1u << 0,
    ZSG2_DIAG_UNALIGNED_ROM_ADDRESS = 1u << 1,
    ZSG2_DIAG_UNKNOWN_CONTROL = 1u << 2
};
typedef struct ZSG2Config {
    uint32_t clock_hz;
    /* Borrowed packed sample bytes, exactly as assembled ROM_REGION32_LE.
     * Size must be nonzero and divisible by 4. ROM remains caller-owned.
     * With external_read, pointer may be NULL, but size still bounds accesses
     * exactly like MAME's required region (G-Net uses its region for length). */
    const uint8_t *sample_rom;
    size_t sample_rom_size;
    uint32_t (*external_read)(void *opaque, uint32_t word_offset);
    void *rom_opaque;
    /* Source output order: reverb, chorus, direct left, direct right.
     * Chip output only: the downstream TMS57002/ZFX-2 is a separate device.
     * Callback sees exact completed-frame cycle for clocked runs; explicit
     * offline render batches retain the device's current cycle. No reentry. */
    void (*sample)(void *opaque, const int16_t outputs[ZSG2_OUTPUTS], uint64_t cycle);
    void *sample_opaque;
    void (*diagnostic)(void *opaque, uint32_t code, uint32_t offset, uint16_t value);
    void *diagnostic_opaque;
} ZSG2Config;
typedef struct ZSG2Status {
    uint32_t clock_hz, sample_rate, rom_words, read_address;
    uint32_t sample_count; /* Literal upstream stream-update batch counter. */
    uint32_t diagnostics;
    uint64_t diagnostic_events, cycles, frames;
    uint64_t sample_phase, periodic_phase;
    int pending_batch;
} ZSG2Status;
typedef struct ZSG2Channel {
    uint16_t registers[16], status;
    uint32_t current_position, step_pointer, step, start, end, loop, page;
    uint16_t volume, initial_volume, target_volume;
    int16_t volume_delta;
    uint16_t cutoff, initial_cutoff, target_cutoff;
    int16_t cutoff_delta;
    int32_t emphasis_filter, output_filter;
    uint8_t output_gain[4];
    int16_t samples[5];
} ZSG2Channel;

int zsg2_create(const ZSG2Config *config, ZSG2 **out);
void zsg2_destroy(ZSG2 *device);
/* Source reset: key-off, clear channel register-derived fields/read address,
 * reset sample_count. Retains cache, filter/sample history, position/step
 * phase, unknown control registers and standalone absolute clock/grid. */
int zsg2_reset(ZSG2 *device);
/* Word offsets, not byte addresses: channels 0x000..2ff, control 0x300..31f.
 * Source rejects partial masks before stream update: writes do nothing,
 * reads return zero, diagnostic is retained, result MASK_UNSUPPORTED. */
int zsg2_read(ZSG2 *device, uint32_t offset, uint16_t mask, uint16_t *value);
int zsg2_write(ZSG2 *device, uint32_t offset, uint16_t value, uint16_t mask);
/* Clocked host model: floor(clock/768) completed frames on a rational grid.
 * Source batch counter changes at 50Hz manager boundaries and valid bus
 * accesses, not arbitrary run slices. Equal MMIO/update histories produce
 * equal samples under whole/split runs. This is not an assertion that MAME's
 * routed streams always use that batch history or completed-frame policy. */
int zsg2_run_cycles(ZSG2 *device, uint64_t cycles);
int zsg2_advance_to_cycle(ZSG2 *device, uint64_t cycle);
/* Explicit source stream-update barrier: end a nonempty pending batch. */
int zsg2_stream_update(ZSG2 *device);
/* Literal sound_stream_update batch, including one sample_count increment
 * for a zero-frame explicit call. No absolute time advancement. Optional
 * interleaved four-channel buffer; capacity measured in frames. */
int zsg2_render_update(ZSG2 *device, size_t frames, int16_t *outputs, size_t capacity);
void zsg2_get_status(const ZSG2 *device, ZSG2Status *status);
int zsg2_get_channel(const ZSG2 *device, unsigned channel, ZSG2Channel *state);
/* Side-effecting decode/cache diagnostic; preserves source zero-block alias. */
int zsg2_decode_block(ZSG2 *device, uint32_t word_offset, int16_t samples[4]);
#ifdef __cplusplus
}
#endif
#endif
