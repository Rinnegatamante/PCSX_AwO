/* Copyright (c) 2021, Aaron Giles. All rights reserved.
 * BSD 3-Clause; see arcade/ymfm/LICENSE.
 */
#ifndef PCSX_ARCADE_YM2610_H
#define PCSX_ARCADE_YM2610_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct YM2610Device YM2610Device;

typedef enum YM2610Model {
    YM2610_MODEL_YM2610 = 0,
    YM2610_MODEL_YM2610B = 1
} YM2610Model;

typedef enum YM2610RomBank {
    YM2610_ROM_ADPCM_A = 0,
    YM2610_ROM_ADPCM_B = 1
} YM2610RomBank;

typedef struct YM2610Config {
    uint32_t model; /* one of YM2610Model; invalid values are rejected */
    uint32_t clock_hz;
    /* Borrowed ROM regions, read without copying. If B is omitted and A is
     * present, MAME's wrapper maps A into both address spaces. */
    const uint8_t *adpcm_a;
    size_t adpcm_a_size;
    const uint8_t *adpcm_b;
    size_t adpcm_b_size;
    /* Optional external memory-space read handler overrides the region
     * pointers. Return the byte at the requested chip address. */
    uint8_t (*read_adpcm)(void *opaque, YM2610RomBank bank, uint32_t address);
    void *rom_opaque;
    void (*irq_line)(void *opaque, int asserted);
    void *irq_opaque;
} YM2610Config;

typedef struct YM2610Sample {
    /* MAME's ymfm_ssg_device_base stream order: SSG mono, FM left, FM right. */
    int16_t ssg;
    int16_t fm_left;
    int16_t fm_right;
} YM2610Sample;

enum {
    YM2610_OK = 0,
    YM2610_ARGUMENT = -1,
    YM2610_NOMEM = -2,
    YM2610_BAD_MODEL = -3,
    YM2610_CAPACITY = -4,
    YM2610_TIME_OVERFLOW = -5
};

int ym2610_create(const YM2610Config *config, YM2610Device **out_device);
void ym2610_destroy(YM2610Device *device);
void ym2610_reset(YM2610Device *device);

/* MAME-style four-register bus interface, byte offsets 0..3. Reads and
 * writes happen at the device's current virtual cycle; call run_cycles to
 * bring chip timers/audio up to the desired bus time first. */
uint8_t ym2610_read(YM2610Device *device, uint32_t offset);
void ym2610_write(YM2610Device *device, uint32_t offset, uint8_t value);

/* Advance the chip's exact input-clock cycle domain. The MAME stream rate is
 * floor(clock_hz / 144), with a rational phase in the input-clock domain.
 * A short output buffer fails without advancing state. */
int ym2610_run_cycles(YM2610Device *device, uint64_t cycles,
                      YM2610Sample *samples, size_t capacity,
                      size_t *samples_generated);
uint64_t ym2610_elapsed_cycles(const YM2610Device *device);
uint32_t ym2610_sample_rate(const YM2610Device *device);

#ifdef __cplusplus
}
#endif
#endif
