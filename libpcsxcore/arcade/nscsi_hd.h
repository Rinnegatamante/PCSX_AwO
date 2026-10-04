/* Standalone MAME-derived NSCSI hard-disk command/media backend.
 * Source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright Olivier Galibert. SPDX-License-Identifier: BSD-3-Clause.
 */
#ifndef LIBPCSXCORE_ARCADE_NSCSI_HD_H
#define LIBPCSXCORE_ARCADE_NSCSI_HD_H

#include "nscsi_target.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nscsi_hd nscsi_hd_t;

typedef int (*nscsi_hd_read_sector_cb)(void *opaque, uint32_t lba,
                                       uint8_t *sector, size_t sector_bytes);
typedef int (*nscsi_hd_write_sector_cb)(void *opaque, uint32_t lba,
                                        const uint8_t *sector,
                                        size_t sector_bytes);

typedef struct nscsi_hd_config {
  int media_present;
  uint32_t cylinders;
  uint32_t heads;
  uint32_t sectors_per_track;
  uint32_t sector_bytes;
  void *media_opaque;
  nscsi_hd_read_sector_cb read_sector;
  nscsi_hd_write_sector_cb write_sector;
  /* Optional IDNT inquiry payload from the mounted image, capped at 48 bytes.
   * If absent, a mounted disk uses MAME's ST225N default identity. */
  const uint8_t *inquiry_data;
  size_t inquiry_data_bytes;
} nscsi_hd_config_t;

enum {
  NSCSI_HD_FAULT_MEDIA_READ = 1u << 0,
  NSCSI_HD_FAULT_MEDIA_WRITE = 1u << 1,
  NSCSI_HD_FAULT_UNINITIALIZED_PREFLIGHT = 1u << 2,
  NSCSI_HD_FAULT_REENTRANT = 1u << 3,
  NSCSI_HD_FAULT_INVALID_CONFIG = 1u << 4,
  NSCSI_HD_FAULT_BACKEND = 1u << 5
};

typedef struct nscsi_hd_status {
  uint32_t faults;
  uint64_t commands;
  uint64_t read_failures;
  uint64_t write_failures;
  uint32_t last_lba;
  uint8_t last_sense[18];
  uint8_t block_known;
} nscsi_hd_status_t;

/* Creates a directly connected SCSI target. No endpoint is attached and NULL
 * is returned when media_present is false, matching MAME's m_scsi_id = -1. */
nscsi_hd_t *nscsi_hd_create(NscsiBus *bus, unsigned scsi_id,
                            const nscsi_hd_config_t *config);
void nscsi_hd_destroy(nscsi_hd_t *device);
/* Borrowed target pointer; it must not be independently destroyed. */
NscsiTarget *nscsi_hd_target(nscsi_hd_t *device);
void nscsi_hd_advance_to(nscsi_hd_t *device, uint64_t now_ns);
void nscsi_hd_reset(nscsi_hd_t *device);
void nscsi_hd_get_status(const nscsi_hd_t *device, nscsi_hd_status_t *status);

#ifdef __cplusplus
}
#endif
#endif
