/* SPDX-License-Identifier: BSD-3-Clause
 * Standalone interface by the pcsx_aow contributors; engine copyright Vas Crabb. */
#ifndef PCSX_ARCADE_DSP16_H
#define PCSX_ARCADE_DSP16_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DSP16 DSP16;
enum { DSP16_MODEL_16 = 0, DSP16_MODEL_16A = 1 };
enum {
    DSP16_INPUT_INT = 0,
    DSP16_INPUT_ICK,
    DSP16_INPUT_ILD,
    DSP16_INPUT_OCK,
    DSP16_INPUT_OLD,
    DSP16_INPUT_EXM
};
enum {
    DSP16_PIN_IACK = 0,
    DSP16_PIN_ICK,
    DSP16_PIN_ILD,
    DSP16_PIN_DO,
    DSP16_PIN_OCK,
    DSP16_PIN_OLD,
    DSP16_PIN_OSE,
    DSP16_PIN_PSEL,
    DSP16_PIN_PIDS,
    DSP16_PIN_PODS
};
enum { DSP16_DIAG_WARNING = 0, DSP16_DIAG_FAULT = 1 };
/* All addresses and ROM sizes are in 16-bit words, with host-native logical values. */
typedef struct DSP16Config {
    int model;
    const uint16_t *rom;
    size_t rom_words; /* Required exactly 0x800 / 0x1000; copied. */
    void *opaque;
    uint16_t (*external_read)(void *, uint16_t word_address);
    uint16_t (*pio_read)(void *, unsigned select);
    void (*pio_write)(void *, unsigned select, uint16_t value);
    void (*pin_write)(void *, unsigned pin, int level);
    void (*parallel_bus_write)(void *, unsigned select, uint16_t value, uint16_t drive_mask);
    void (*diagnostic)(void *, int severity, uint16_t pc, const char *message);
} DSP16Config;
typedef struct DSP16Registers {
    uint16_t pc, pt, pr, pi;
    int16_t i;
    uint16_t r[4], rb, re;
    int16_t j, k;
    int16_t x;
    int32_t y, p;
    int64_t a[2];
    int8_t c[3];
    uint8_t auc;
    uint16_t psw;
} DSP16Registers;
typedef struct DSP16Status {
    DSP16Registers registers;
    uint64_t cycles, instructions, warnings, faults;
    uint16_t instruction_pc, sioc, pioc, serial_buffer, parallel_input, parallel_output;
    uint8_t phase, cache_mode, cache_pointer, cache_limit, cache_iterations;
    uint8_t pins[10], halted;
} DSP16Status;
DSP16 *dsp16_create(const DSP16Config *);
void dsp16_destroy(DSP16 *);
void dsp16_reset(DSP16 *); /* Hardware reset: retains RAM and registers not reset upstream. */
/* Exact machine-cycle budget, 2 crystal clocks/cycle; may stop mid-instruction. No debt. */
uint64_t dsp16_run_cycles(DSP16 *, uint64_t cycles);
int dsp16_set_input(DSP16 *, unsigned input, int level);
uint16_t dsp16_host_pio_read(DSP16 *);
void dsp16_host_pio_write(DSP16 *, uint16_t);
int dsp16_get_status(const DSP16 *, DSP16Status *);
int dsp16_get_registers(const DSP16 *, DSP16Registers *);
/* Restarts the fetch pipeline/cache; preserves peripherals and counters. */
int dsp16_set_registers(DSP16 *, const DSP16Registers *);
/* R encodings from the ISA (0..0x1e); accesses have real peripheral side effects. */
int dsp16_read_register(DSP16 *, unsigned r, uint16_t *);
int dsp16_write_register(DSP16 *, unsigned r, uint16_t);
uint16_t dsp16_ram_read(const DSP16 *, uint16_t word_address);
void dsp16_ram_write(DSP16 *, uint16_t word_address, uint16_t);
#ifdef __cplusplus
}
#endif
#endif
