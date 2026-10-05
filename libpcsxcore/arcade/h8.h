/* SPDX-License-Identifier: BSD-3-Clause
 * H8/3002 standalone execution interface. ISA derived from MAME, Olivier Galibert.
 * Bus callbacks use logical byte addresses in the H8 big-endian address space.
 */
#ifndef PCSX_ARCADE_H8_H
#define PCSX_ARCADE_H8_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct H8CPU H8CPU;
typedef struct H8Config {
    void *opaque;
    uint8_t (*read8)(void *opaque, uint32_t address);
    void (*write8)(void *opaque, uint32_t address, uint8_t value);
    /* H8 port numbers: 4, 6, 7, 8, 9, 10 (A), 11 (B). Optional. */
    uint8_t (*port_read)(void *opaque, unsigned port);
    void (*port_write)(void *opaque, unsigned port, uint8_t value, uint8_t ddr);
    void (*serial_tx)(void *opaque, unsigned channel, uint8_t value);
    /* Optional atomic external word accesses (e.g. C352 execution register). */
    uint16_t (*read16)(void *opaque, uint32_t address);
    void (*write16)(void *opaque, uint32_t address, uint16_t value);
    /* Raw 10-bit analog input, channels 0..7. Unconnected defaults to zero. */
    uint16_t (*adc_read)(void *opaque, unsigned channel);
} H8Config;
typedef struct H8Status {
    uint32_t pc, er[8];
    uint64_t cycles, instructions;
    uint32_t illegal_pc;
    uint16_t illegal_opcode;
    uint8_t ccr, sleeping, halted;
    /* Unsupported peripheral accesses: bit 0 DMA, bit 1 watchdog. */
    uint32_t unsupported_peripherals;
} H8Status;
H8CPU *h8_create(const H8Config *config);
void h8_destroy(H8CPU *cpu);
void h8_reset(H8CPU *cpu);
/* Executes whole instructions, carries cycle overshoot into the next call. */
void h8_run_cycles(H8CPU *cpu, unsigned cycles);
/* Current emulated H8 cycle, including access/internal costs already charged in
 * the executing state. Callbacks may query this without changing CPU state.
 * Outside state execution (including SCI/ADC tick callbacks and debug accesses)
 * returns committed cycles; reset restarts at zero. NULL returns zero. */
uint64_t h8_current_cycle(const H8CPU *cpu);
/* IRQ0..IRQ5 are 0..5; NMI is -1. Level is 0/1 (asserted). */
void h8_set_irq(H8CPU *cpu, int line, int level);
void h8_serial_receive(H8CPU *cpu, unsigned channel, uint8_t value);
/* Physical ADTRG pin level; falling edge triggers when ADCR bit7 is set. */
void h8_adc_trigger(H8CPU *cpu, int level);
void h8_get_status(const H8CPU *cpu, H8Status *status);
/* Debug bus accesses include internal RAM/registers and do not spend cycles. */
uint8_t h8_read_byte(H8CPU *cpu, uint32_t address);
void h8_write_byte(H8CPU *cpu, uint32_t address, uint8_t value);
#ifdef __cplusplus
}
#endif
#endif
