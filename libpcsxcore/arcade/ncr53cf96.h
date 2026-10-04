/*
 * Standalone NCR 53CF96-2 controller core, based on MAME's
 * src/devices/machine/ncr53c90.cpp and ncr53c90.h.
 * MAME revision: deb2f54fe7300e7a192690318beb860159bfd52a
 * Copyright Olivier Galibert.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LIBPCSXCORE_ARCADE_NCR53CF96_H
#define LIBPCSXCORE_ARCADE_NCR53CF96_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ncr53cf96 Ncr53cf96;

enum {
    NCR53CF96_S_INP = 0x001,
    NCR53CF96_S_CTL = 0x002,
    NCR53CF96_S_MSG = 0x004,
    NCR53CF96_S_BSY = 0x008,
    NCR53CF96_S_SEL = 0x010,
    NCR53CF96_S_REQ = 0x020,
    NCR53CF96_S_ACK = 0x040,
    NCR53CF96_S_ATN = 0x080,
    NCR53CF96_S_RST = 0x100,
    NCR53CF96_S_PHASE_MASK = 0x007,
    NCR53CF96_PHASE_DATA_OUT = 0x000,
    NCR53CF96_PHASE_DATA_IN = 0x001,
    NCR53CF96_PHASE_COMMAND = 0x002,
    NCR53CF96_PHASE_STATUS = 0x003,
    NCR53CF96_PHASE_MSG_OUT = 0x006,
    NCR53CF96_PHASE_MSG_IN = 0x007
};

typedef struct ncr53cf96_bus_ops {
    void *opaque;
    /* Controller drives open-collector control/data lines. External-device
     * levels are returned later with ncr53cf96_bus_changed(); this event must
     * not call back into the controller synchronously. */
    void (*drive_ctrl)(void *opaque, uint32_t lines, uint32_t mask);
    void (*drive_data)(void *opaque, uint32_t bits);
    void (*wait_ctrl)(void *opaque, uint32_t lines, uint32_t mask);
    void (*irq)(void *opaque, int asserted);
    void (*drq)(void *opaque, int asserted);
} ncr53cf96_bus_ops_t;

typedef struct ncr53cf96_status {
    uint64_t cycles;
    uint64_t timer_deadline;
    uint32_t transfer_count, transfer_counter;
    uint32_t bus_ctrl, bus_data;
    uint8_t command, status, interrupt_status, sequence, fifo_count;
    uint8_t phase, mode, config, config2, config3, config4;
    uint8_t irq, drq, dma_direction, selected_target, scsi_id, test_mode;
    uint32_t faults;
} ncr53cf96_status_t;

enum {
    NCR53CF96_FAULT_ARBITRATION_LOST = 1u << 0,
    NCR53CF96_FAULT_TIME_REWIND = 1u << 1,
    NCR53CF96_FAULT_TIME_OVERFLOW = 1u << 2
};

/* `clock_hz` is the chip clock. Caller owns bus and must provide line-change
 * notifications via ncr53cf96_bus_changed(), never a fabricated target reply. */
Ncr53cf96 *ncr53cf96_create(uint32_t clock_hz, const ncr53cf96_bus_ops_t *ops);
void ncr53cf96_destroy(Ncr53cf96 *chip);
void ncr53cf96_reset(Ncr53cf96 *chip);

/* NCR register slot 0..15; on the 32-byte PSX aperture the board converts
 * each low-byte-lane word slot to this index. Out-of-range indices are ignored.
 * Writes to register 15 control 16-bit FIFO alignment (CF94 behavior). */
uint8_t ncr53cf96_read(Ncr53cf96 *chip, unsigned reg);
void ncr53cf96_write(Ncr53cf96 *chip, unsigned reg, uint8_t value);

/* Chip-clock time. Absolute, monotonic cycle values; deterministic and
 * independent of host wall clock. */
void ncr53cf96_advance_to(Ncr53cf96 *chip, uint64_t cycles);

/* DMA host interface, one byte/word at a time. */
uint8_t ncr53cf96_dma_read(Ncr53cf96 *chip);
void ncr53cf96_dma_write(Ncr53cf96 *chip, uint8_t value);
uint16_t ncr53cf96_dma16_read(Ncr53cf96 *chip);
void ncr53cf96_dma16_write(Ncr53cf96 *chip, uint16_t value);

/* External SCSI bus input event (levels driven by targets/other initiators,
 * excluding the controller's own drive), delivered when the bus resolver's
 * ctrl_wait policy would notify this endpoint. Bit definitions match MAME
 * NSCSI. The event immediately steps unless already in a step; in that case
 * the new sample is consumed on the next step/tick. */
void ncr53cf96_bus_changed(Ncr53cf96 *chip, uint32_t ctrl, uint32_t data);
void ncr53cf96_get_status(const Ncr53cf96 *chip, ncr53cf96_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
