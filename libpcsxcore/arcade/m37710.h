/* Standalone M37702/M37710, BSD-3-Clause, R. Belmont, Karl Stenerud.
 * See m37710_LICENSE.txt. CPU cycles are oscillator clocks / 2. */
#ifndef PCSX_ARCADE_M37710_H
#define PCSX_ARCADE_M37710_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "m37710_enums.inc"
typedef struct M37710 M37710;
enum { M37710_MODEL_M37702M2, M37710_MODEL_M37702S1, M37710_MODEL_M37710S4 };
enum { M37710_DIAG_ILLEGAL = 1, M37710_DIAG_PERIPHERAL = 2 };
typedef struct M37710Config {
    unsigned model;
    void *opaque;
    uint8_t (*read8)(void *, uint32_t address);
    void (*write8)(void *, uint32_t address, uint8_t value);
    /* Optional aligned atomic external little-endian word accesses. Unaligned words use bytes. */
    uint16_t (*read16)(void *, uint32_t address);
    void (*write16)(void *, uint32_t address, uint16_t value);
    uint8_t (*port_read)(void *, unsigned port, uint8_t input_mask);
    void (*port_write)(void *, unsigned port, uint8_t value, uint8_t output_mask);
    uint16_t (*analog_read)(void *, unsigned channel);
    void (*irq_ack)(void *, unsigned line);
    void (*diagnostic)(void *, unsigned kind, uint32_t pc, uint32_t detail);
    const uint8_t *internal_rom; /* Required exactly 16384 bytes for M37702M2. Copied. */
    size_t internal_rom_size;
} M37710Config;
typedef struct M37710Status {
    uint32_t pc, previous_pc, pending_irqs;
    uint16_t a, b, x, y, s, dpr, ps;
    uint8_t pg, dt, stopped, fault;
    uint64_t cycles, instructions, interrupts, illegal_opcodes, unsupported_accesses;
    uint64_t timer_ticks[8];
} M37710Status;
M37710 *m37710_create(const M37710Config *config);
void m37710_destroy(M37710 *cpu);
void m37710_reset(M37710 *cpu);
/* Instruction-boundary execution; retains overshoot debt across calls. Return actual cycles. */
uint64_t m37710_run_cycles(M37710 *cpu, uint64_t cycles);
void m37710_irq(M37710 *cpu, unsigned line, int asserted);
/* Event-counter input: one tick per asserted call, as upstream MAME. */
void m37710_timer_input(M37710 *cpu, unsigned timer, int asserted);
void m37710_timer_direction(M37710 *cpu, unsigned timer, int up);
void m37710_get_status(const M37710 *cpu, M37710Status *status);
uint32_t m37710_get_register(M37710 *cpu, unsigned reg);
void m37710_set_register(M37710 *cpu, unsigned reg, uint32_t value);
uint8_t m37710_read8(M37710 *cpu, uint32_t address);
uint16_t m37710_read16(M37710 *cpu, uint32_t address);
void m37710_write8(M37710 *cpu, uint32_t address, uint8_t value);
void m37710_write16(M37710 *cpu, uint32_t address, uint16_t value);
#ifdef __cplusplus
}
#endif
#endif
