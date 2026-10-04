/* SPDX-License-Identifier: BSD-3-Clause
 * QSound LLE adapter; MAME source copyright Vas Crabb. */
#ifndef PCSX_ARCADE_QSOUND_H
#define PCSX_ARCADE_QSOUND_H
#include "dsp16.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct QSound QSound;
typedef struct QSoundConfig {
    const uint16_t *firmware;
    size_t firmware_words; /* Required 0x1000 logical words, copied. */
    const uint8_t *samples;
    uint32_t sample_bytes; /* Required caller-owned immutable region. */
    uint32_t clock_hz;     /* Crystal clock; zero selects 60 MHz. */
    void *opaque;
    /* Uniform output stream at floor(clock_hz / 2496), holding the recovered DAC samples. */
    void (*pcm)(void *, int16_t left, int16_t right, uint64_t frame);
    /* Actual decoded serial word, with DSP machine-cycle timestamp. */
    void (*dac_write)(void *, unsigned channel, int16_t sample, uint64_t dsp_cycle);
    void (*diagnostic)(void *, int severity, uint16_t dsp_pc, const char *message);
} QSoundConfig;
typedef struct QSoundStatus {
    uint64_t dsp_cycles, pcm_frames, recovered_words[2], sample_reads, commands;
    uint32_t clock_hz, sample_rate, sample_region_bank;
    uint16_t rom_bank, rom_offset, command_address, command_data, data_latch;
    int16_t samples[2];
    uint8_t command_pending, ready, crystal_remainder, halted;
    DSP16Status dsp;
} QSoundStatus;
QSound *qsound_create(const QSoundConfig *);
void qsound_destroy(QSound *);
void qsound_reset(QSound *);
/* Offsets 0/1 = data high/low; 2 = command address. Caller subtracts 0xd000. */
int qsound_write(QSound *, unsigned offset, uint8_t value);
/* Status for Capcom ZN address 0xd007: 0x80 when ready, 0x00 while pending. */
uint8_t qsound_read(const QSound *);
uint64_t qsound_run_cycles(QSound *, uint64_t dsp_machine_cycles);
uint64_t qsound_run_clocks(QSound *, uint64_t crystal_clocks); /* Returns DSP cycles; keeps odd remainder. */
int qsound_set_clock(QSound *, uint32_t crystal_hz);
/* device_rom_interface bank for sample regions >=16 MiB; modulo available whole banks. */
int qsound_set_sample_region_bank(QSound *, unsigned bank);
int qsound_get_status(const QSound *, QSoundStatus *);
/* Debugging access retains real DSP/peripheral side effects; no command shortcuts. */
DSP16 *qsound_get_dsp(QSound *);
#ifdef __cplusplus
}
#endif
#endif
