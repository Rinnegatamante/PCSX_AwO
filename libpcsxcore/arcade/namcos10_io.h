/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holders:smf,windyfairy
 */
#ifndef PCSX_ARCADE_NAMCOS10_IO_H
#define PCSX_ARCADE_NAMCOS10_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NS10IO NS10IO;

typedef enum NS10IOResult {
    NS10_IO_OK = 0,
    NS10_IO_UNMAPPED = -1,
    NS10_IO_ARGUMENT = -2,
    NS10_IO_NOMEM = -3
} NS10IOResult;

typedef struct NS10IOConfig {
    /* Return the raw active-low MAME SYSTEM and IN1 input-port values. */
    uint16_t (*read_system)(void *opaque);
    uint32_t (*read_in1)(void *opaque);
    /* MAME binds the 100 Hz base-board I/O interrupt to PSX INT10. */
    void (*irq10)(void *opaque, int asserted);
    void *opaque;
    /* 0xff represents no EXIO daughterboard. */
    uint8_t exio_ident_code;
} NS10IOConfig;

/* No config means pull-up inputs, no EXIO, and no IRQ callback. */
int ns10_io_create(const NS10IOConfig *config, NS10IO **out_io);
void ns10_io_destroy(NS10IO *io);
void ns10_io_reset(NS10IO *io);

/* Physical address, 1/2/4-byte widths, little-endian CPU value. KSEG aliases
 * are canonicalized. Write mem_mask has MAME's ones-mean-written semantics. */
int ns10_io_read(NS10IO *io, uint32_t address, unsigned width, uint32_t *value);
int ns10_io_write(NS10IO *io, uint32_t address, unsigned width,
                  uint32_t value, uint32_t mem_mask);

/* Advance the base-board's 100 Hz timer (immediate first tick after reset).
 * Each tick sets I/O pending bit 3 and
 * calls irq10(1), matching the MAME timer callback. */
void ns10_io_advance_us(NS10IO *io, uint64_t elapsed_us);
/* ATA/CD IRQ input from the external drive: source pending bit 2 and INT10. */
void ns10_io_cdrom_interrupt(NS10IO *io, int state);

#ifdef __cplusplus
}
#endif
#endif
