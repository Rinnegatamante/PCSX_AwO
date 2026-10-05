/* RF5C296 PC Card controller interface, adapted from MAME.
 * Source: src/devices/machine/rf5c296.h, revision
 * deb2f54fe7300e7a192690318beb860159bfd52a. Copyright smf.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef PCSX_ARCADE_RF5C296_H
#define PCSX_ARCADE_RF5C296_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rf5c296_device rf5c296_device_t;

/* Borrowed callbacks following MAME's PC Card interface. read_memory receives
 * the decoded I/O byte/register offset; read_reg receives the x16 aperture
 * offset. Data and masks are 16-bit bus values. */
typedef struct rf5c296_card_ops {
    void *opaque;
    uint16_t (*read_memory)(void *opaque, uint32_t offset, uint16_t mem_mask);
    void (*write_memory)(void *opaque, uint32_t offset, uint16_t data,
                         uint16_t mem_mask);
    uint16_t (*read_reg)(void *opaque, uint32_t offset, uint16_t mem_mask);
    void (*write_reg)(void *opaque, uint32_t offset, uint16_t data,
                      uint16_t mem_mask);
    void (*reset)(void *opaque);
} rf5c296_card_ops_t;

rf5c296_device_t *rf5c296_create(const rf5c296_card_ops_t *card);
void rf5c296_destroy(rf5c296_device_t *device);

/* Controller I/O offsets are 16-bit word offsets from the RF5C296 window.
 * MAME doubles the offset and uses the low/high byte lanes at byte offsets
 * 0x3e0/0x3e1 to select/read-or-write the indexed controller register. */
uint16_t rf5c296_io_read(rf5c296_device_t *device, uint32_t offset,
                         uint16_t mem_mask);
void rf5c296_io_write(rf5c296_device_t *device, uint32_t offset,
                      uint16_t data, uint16_t mem_mask);

/* PC Card attribute/configuration memory window used by MAME's mem_r/mem_w. */
uint16_t rf5c296_mem_read(rf5c296_device_t *device, uint32_t offset,
                          uint16_t mem_mask);
void rf5c296_mem_write(rf5c296_device_t *device, uint32_t offset,
                       uint16_t data, uint16_t mem_mask);

#ifdef __cplusplus
}
#endif
#endif
