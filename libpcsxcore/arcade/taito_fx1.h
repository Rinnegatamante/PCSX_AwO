/*
 * Taito FX-1S base daughterboard adapter, based on MAME sony/zn.cpp.
 * Source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 * Copyright smf, Philip Bennett (upstream does not state years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LIBPCSXCORE_ARCADE_TAITO_FX1_H
#define LIBPCSXCORE_ARCADE_TAITO_FX1_H

#include <stddef.h>
#include <stdint.h>

#include "mb3773.h"
#include "taito_sound_comm.h"
#include "ym2610.h"
#include "z80.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAITO_FX1_BANK_WINDOW_SIZE 0x800000u
#define TAITO_FX1_BANK_COUNT 4u
#define TAITO_FX1_AUDIO_BANK_SIZE 0x4000u
#define TAITO_FX1_AUDIO_BANK_COUNT 8u
#define TAITO_FX1_AUDIO_RAM_SIZE 0x2000u
#define TAITO_FX1_AUDIO_CLOCK_HZ 4000000u
#define TAITO_FX1_YM_CLOCK_HZ 8000000u
#define TAITO_FX1_MAIN_BANKED_ROM_SIZE 0x1000000u
#define TAITO_FX1_AUDIO_ROM_SIZE 0x20000u
#define TAITO_FX1_ADPCMA_MIN_SIZE 0x200000u

typedef struct TaitoFx1 TaitoFx1;

typedef enum taito_fx1_result {
    TAITO_FX1_FAULT = -2,
    TAITO_FX1_INVALID = -1,
    TAITO_FX1_UNMAPPED = 0,
    TAITO_FX1_HANDLED = 1
} taito_fx1_result_t;

enum {
    TAITO_FX1_FAULT_MAIN_BANK_BOUNDS = 1u << 0,
    TAITO_FX1_FAULT_SOUND_ROM_BOUNDS = 1u << 1,
    TAITO_FX1_FAULT_YM2610 = 1u << 2,
    TAITO_FX1_FAULT_TIME = 1u << 3,
    TAITO_FX1_FAULT_SOUND_UNMAPPED = 1u << 4
};

typedef struct taito_fx1_callbacks {
    void *opaque;
    /* Scheduler request only: do not run the Z80 reentrantly from this callback. */
    void (*request_sound_synchronization)(void *opaque);
    /* Queue a deferred machine soft reset; do not reset/reenter this adapter. */
    void (*request_soft_reset)(void *opaque);
    /* Optional mixed stereo YM2610B stream at 8 MHz / 144 (integer 55,555 Hz). */
    void (*audio_sample)(void *opaque, int16_t left, int16_t right);
} taito_fx1_callbacks_t;

typedef struct taito_fx1_config {
    /* Borrowed, source-sized MAME regions. The adapter does not free them. */
    const uint8_t *banked_rom; /* `bankedroms`, 16 MiB in all FX1S game profiles. */
    size_t banked_rom_size;
    const uint8_t *audio_rom;  /* `audiocpu`, 128 KiB in FX1S game profiles. */
    size_t audio_rom_size;
    const uint8_t *adpcm_a;    /* `ym2610b:adpcma`, 2 or 4 MiB; also YM2610B ADPCM-B map. */
    size_t adpcm_a_size;
} taito_fx1_config_t;

typedef struct taito_fx1_status {
    unsigned main_bank;
    size_t main_bank_offset;
    unsigned sound_bank;
    size_t sound_bank_offset;
    int sound_reset_asserted;
    int nmi_asserted;
    int ym_irq_asserted;
    int synchronization_pending;
    int soft_reset_pending;
    uint64_t sound_cycles;
    uint64_t ym_cycles;
    uint64_t ym_bus_events;
    uint64_t last_ym_bus_sound_time;
    uint64_t audio_samples;
    uint64_t watchdog_time_us;
    uint64_t watchdog_reset_requests;
    uint64_t synchronization_requests;
    uint32_t faults;
    uint8_t tc0140_status;
    int z80_halted;
    uint16_t z80_pc;
    uint64_t z80_instructions;
} taito_fx1_status_t;

/* Requires all source-sized ROM regions and callbacks for deferred reset and
 * scheduler synchronization. YM2610B, TC0140SYT, MB3773 and Z80 are owned by
 * the returned board; ROM data and callback context remain borrowed. */
TaitoFx1 *taito_fx1_create(const taito_fx1_config_t *config,
                           const taito_fx1_callbacks_t *callbacks);
void taito_fx1_destroy(TaitoFx1 *board);
/* MAME machine reset: bank zero, sound bank one, reset CPU/chip comm state.
 * RAM contents are retained, like the mapped MAME RAM device. */
void taito_fx1_reset(TaitoFx1 *board);

/* Monotonic absolute machine time for MB3773. Expiry only schedules the
 * caller's deferred soft-reset callback after the current board call returns. */
int taito_fx1_advance_to_us(TaitoFx1 *board, uint64_t now_us);
/* Acknowledge that the host has serviced a previously queued scheduler barrier. */
void taito_fx1_sound_synchronization_completed(TaitoFx1 *board);

/* Physical PSX daughterboard map. Width is 1/2/4, naturally aligned, and
 * writes/read bytes use little-endian bus lanes. Unmapped addresses belong to
 * the Sony ZN common map. Bank selections 2/3 are retained, but accesses fault
 * when they exceed the 16 MiB MAME region rather than reading fabricated data. */
taito_fx1_result_t taito_fx1_main_read(TaitoFx1 *board, uint32_t address,
                                      unsigned width, uint32_t *value);
taito_fx1_result_t taito_fx1_main_write(TaitoFx1 *board, uint32_t address,
                                       unsigned width, uint32_t value);

/* Run nominal 4 MHz Z80 T-states. The YM2610B advances at twice that clock;
 * one-T-state scheduler slices resolve host synchronization to two YM clocks. */
taito_fx1_result_t taito_fx1_run_sound_cycles(TaitoFx1 *board, uint64_t cycles);

/* Side-effecting sound bus helpers, primarily for diagnostics and tests. */
uint8_t taito_fx1_sound_read(TaitoFx1 *board, uint16_t address);
void taito_fx1_sound_write(TaitoFx1 *board, uint16_t address, uint8_t value);
uint8_t taito_fx1_sound_port_read(TaitoFx1 *board, uint16_t port);
void taito_fx1_sound_port_write(TaitoFx1 *board, uint16_t port, uint8_t value);
void taito_fx1_get_status(const TaitoFx1 *board, taito_fx1_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
