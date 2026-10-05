/* G-Net ATA flash PC Card behavior, adapted from MAME ataflash.cpp.
 * Source revision: deb2f54fe7300e7a192690318beb860159bfd52a.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef PCSX_ARCADE_GNET_PCCARD_H
#define PCSX_ARCADE_GNET_PCCARD_H

#include "rf5c296.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gnet_pccard gnet_pccard_t;

typedef enum gnet_pccard_variant {
    GNET_PCCARD_ATA_FLASH = 0,
    GNET_PCCARD_TAITO_TYPE1 = 1,
    GNET_PCCARD_TAITO_TYPE2 = 2,
    GNET_PCCARD_TAITO_COMPACT_FLASH = 3
} gnet_pccard_variant_t;

typedef enum gnet_pccard_result {
    GNET_PCCARD_OK = 0,
    GNET_PCCARD_NO_ATA_BACKEND = 1,
    GNET_PCCARD_ARGUMENT = -1,
    GNET_PCCARD_NOMEM = -2,
    GNET_PCCARD_BUSY = -3
} gnet_pccard_result_t;

/* Status bits and error values used by MAME's ata_hle_device_base. */
enum {
    GNET_PCCARD_STATUS_ERR = 0x01,
    GNET_PCCARD_STATUS_DRQ = 0x08,
    GNET_PCCARD_STATUS_DRDY = 0x40,
    GNET_PCCARD_ERROR_NONE = 0x00,
    GNET_PCCARD_IRQ_ASSERT = 1
};

/* Snapshot presented by an ATA backend at its process_command() hook. */
typedef struct gnet_pccard_taskfile {
    uint8_t command;
    uint8_t feature;
    /* MAME HLE expands the ATA register value 0 to internal count 256. */
    uint16_t sector_count;
    uint8_t sector_number;
    uint8_t cylinder_low;
    uint8_t cylinder_high;
    uint8_t status;
} gnet_pccard_taskfile_t;

/* Apply the returned delta to the ATA engine's live state. For a PASS result,
 * the backend runs its normal command/buffer path. HANDLED means the card
 * consumed the event according to MAME's ataflash implementation. */
typedef struct gnet_pccard_state_delta {
    uint8_t status_set;
    uint8_t status_clear;
    uint8_t error;
    uint8_t error_valid;
    uint8_t irq;
    uint8_t irq_valid;
    uint16_t sector_count;
    uint8_t sector_count_valid;
    uint16_t transfer_bytes;
    uint8_t transfer_bytes_valid;
} gnet_pccard_state_delta_t;

typedef enum gnet_pccard_hook_result {
    GNET_PCCARD_HOOK_ERROR = -1,
    GNET_PCCARD_HOOK_PASS = 0,
    GNET_PCCARD_HOOK_HANDLED = 1
} gnet_pccard_hook_result_t;

/* These operations are borrowed. They correspond to the ATA HLE's existing
 * command_r/w and control_r/w entry points. That engine owns ATA registers,
 * status, IRQ/timing, normal data buffering, and sector media. Its
 * process_command/process_buffer/is_ready hooks must call the matching
 * gnet_pccard_* functions below. The command/buffer/readiness hooks are the
 * sole sanctioned synchronous callback reentry. Other reentry is unsupported;
 * callbacks must not destroy the card. */
typedef struct gnet_pccard_ata_ops {
    void *opaque;
    void (*reset)(void *opaque);
    uint16_t (*command_r)(void *opaque, uint32_t offset, uint16_t mem_mask,
                          int eight_bit_data_transfers);
    void (*command_w)(void *opaque, uint32_t offset, uint16_t data,
                      uint16_t mem_mask, int eight_bit_data_transfers);
    uint16_t (*control_r)(void *opaque, uint32_t offset, uint16_t mem_mask);
    void (*control_w)(void *opaque, uint32_t offset, uint16_t data,
                      uint16_t mem_mask);
} gnet_pccard_ata_ops_t;

typedef struct gnet_pccard_config {
    gnet_pccard_variant_t variant;
    const uint8_t *cis;
    size_t cis_size;
    /* MAME obtains this five-byte key from disk-image metadata. A locked
     * Taito card is enabled only when key_size is exactly five. */
    const uint8_t *key;
    size_t key_size;
    /* Descriptor is copied; opaque pointers in its callbacks are borrowed. */
    const gnet_pccard_ata_ops_t *ata;
} gnet_pccard_config_t;

int gnet_pccard_create(const gnet_pccard_config_t *config,
                       gnet_pccard_t **out);
int gnet_pccard_try_destroy(gnet_pccard_t *card);
void gnet_pccard_destroy(gnet_pccard_t *card);
int gnet_pccard_reset(gnet_pccard_t *card);
int gnet_pccard_has_ata_backend(const gnet_pccard_t *card);
int gnet_pccard_is_locked(const gnet_pccard_t *card);
int gnet_pccard_is_ready(const gnet_pccard_t *card);

/* Stable descriptor for rf5c296_create; it contains opaque=card. */
const rf5c296_card_ops_t *gnet_pccard_card_ops(gnet_pccard_t *card);

/* Call at MAME's ata_hle_device_base::process_command() point before normal
 * ATA dispatch. Type-2 unlock buffer completion must similarly be offered at
 * process_buffer() before normal media handling. */
gnet_pccard_hook_result_t gnet_pccard_process_command(
    gnet_pccard_t *card, const gnet_pccard_taskfile_t *taskfile,
    gnet_pccard_state_delta_t *delta);
gnet_pccard_hook_result_t gnet_pccard_process_buffer(
    gnet_pccard_t *card, uint8_t command, const uint8_t *buffer,
    size_t buffer_size, gnet_pccard_state_delta_t *delta);

/* MAME's ata_flash_pccard_device::ide_build_identify_device sets word 0 to
 * 0x848a after the CF base has built the identify buffer. */
int gnet_pccard_identify_word0(gnet_pccard_t *card, uint16_t *word0);

#ifdef __cplusplus
}
#endif
#endif
