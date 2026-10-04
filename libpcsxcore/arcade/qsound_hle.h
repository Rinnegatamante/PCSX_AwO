/* SPDX-License-Identifier: BSD-3-Clause
 * QSound HLE adapter; MAME source copyright superctr, Valley Bell. */
#ifndef PCSX_ARCADE_QSOUND_HLE_H
#define PCSX_ARCADE_QSOUND_HLE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct QSoundHLE QSoundHLE;
typedef struct QSoundHLEConfig {
    const uint16_t *firmware;
    size_t firmware_words; /* Required 0x1000 logical words, copied, including coefficients. */
    const uint8_t *samples;
    uint32_t sample_bytes; /* Caller-owned immutable sample region. */
    uint32_t clock_hz;     /* Zero selects 60 MHz. */
    void *opaque;
    void (*pcm)(void *, int16_t left, int16_t right, uint64_t frame);
    void (*diagnostic)(void *, const char *message);
} QSoundHLEConfig;
typedef struct QSoundHLEStatus {
    uint64_t frames, cycles, commands;
    uint32_t clock_hz, sample_rate, sample_region_bank;
    uint16_t state, next_state, data_latch, echo_end, delay_update;
    int16_t output[2], voice_output[19], echo_length;
    uint8_t ready, state_counter, crystal_remainder;
    uint16_t filter_table[2], alt_filter_table[2];
    int filter_taps[2], alt_filter_taps[2];
} QSoundHLEStatus;
QSoundHLE *qsound_hle_create(const QSoundHLEConfig *);
void qsound_hle_destroy(QSoundHLE *);
void qsound_hle_reset(QSoundHLE *);
/* Same offsets as LLE: 0/1 data high/low, 2 command. Caller subtracts 0xd000. */
int qsound_hle_write(QSoundHLE *, unsigned offset, uint8_t value);
uint8_t qsound_hle_read(const QSoundHLE *);
uint64_t qsound_hle_run_cycles(QSoundHLE *, uint64_t dsp_machine_cycles);
uint64_t qsound_hle_run_clocks(QSoundHLE *, uint64_t crystal_clocks);
/* Execute explicit stream samples, for deterministic offline rendering. */
void qsound_hle_run_frames(QSoundHLE *, uint64_t frames);
int qsound_hle_set_clock(QSoundHLE *, uint32_t crystal_hz);
int qsound_hle_set_sample_region_bank(QSoundHLE *, unsigned bank);
int qsound_hle_get_status(const QSoundHLE *, QSoundHLEStatus *);
/* Inspect actual mapped DSP command registers, without modifying readiness. */
int qsound_hle_read_register(const QSoundHLE *, unsigned command_address, uint16_t *value);
#ifdef __cplusplus
}
#endif
#endif
