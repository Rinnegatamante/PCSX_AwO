/* Standalone Panasonic MN10200 / MN1020012A core API.
 * Derived from MAME mn10200.cpp; see mn10200_LICENSE.txt.
 */
#ifndef PCSX_ARCADE_MN10200_H
#define PCSX_ARCADE_MN10200_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mn10200_cpu mn10200_cpu;

typedef uint8_t (*mn10200_read8_fn)(void *opaque, uint32_t address);
typedef void (*mn10200_write8_fn)(void *opaque, uint32_t address, uint8_t value);
/* Optional aligned external word handlers.  A 16-bit data access is one bus
 * transaction when these are supplied; absent handlers retain the historical
 * two-byte callback behavior.  The internal 0xfc00 register window always
 * remains decoded by the CPU core. */
typedef uint16_t (*mn10200_read16_fn)(void *opaque, uint32_t address);
typedef void (*mn10200_write16_fn)(void *opaque, uint32_t address, uint16_t value);
typedef uint8_t (*mn10200_port_read_fn)(void *opaque, unsigned port, uint8_t mask);
typedef void (*mn10200_port_write_fn)(void *opaque, unsigned port, uint8_t value, uint8_t mask);

typedef struct mn10200_config {
    void *opaque;
    mn10200_read8_fn read8;
    mn10200_write8_fn write8;
    mn10200_port_read_fn port_read;
    mn10200_port_write_fn port_write;
    mn10200_read16_fn read16;
    mn10200_write16_fn write16;
} mn10200_config;

/* The 12A internal register window is decoded at 0x00fc00-0x00ffff before
 * the caller's 24-bit little-endian external bus. DMA/serial registers follow
 * MAME's current CPU implementation; it stores their state but does not
 * implement transfer engines or wire-level serial engines. */
mn10200_cpu *mn10200_create(const mn10200_config *config);
void mn10200_destroy(mn10200_cpu *cpu); /* forbidden from a bus/port callback */
int mn10200_reset(mn10200_cpu *cpu);
/* Runs a positive internal-cycle budget; returns consumed cycles (may exceed
 * budget by one instruction and interrupt entry), or a negative error. Timer callbacks run at
 * source-derived deadlines. */
int64_t mn10200_run(mn10200_cpu *cpu, uint64_t cycles);
int mn10200_set_irq(mn10200_cpu *cpu, unsigned line, int asserted);
uint32_t mn10200_get_pc(const mn10200_cpu *cpu);
uint16_t mn10200_get_psw(const mn10200_cpu *cpu);
uint32_t mn10200_get_d(const mn10200_cpu *cpu, unsigned index);
uint32_t mn10200_get_a(const mn10200_cpu *cpu, unsigned index);
uint64_t mn10200_get_cycles(const mn10200_cpu *cpu);
uint64_t mn10200_get_callback_cycle(const mn10200_cpu *cpu);
uint32_t mn10200_get_fault(const mn10200_cpu *cpu);
void mn10200_clear_fault(mn10200_cpu *cpu);
uint8_t mn10200_read_internal(mn10200_cpu *cpu, uint16_t offset);
int mn10200_write_internal(mn10200_cpu *cpu, uint16_t offset, uint8_t value);

#ifdef __cplusplus
}
#endif
#endif
