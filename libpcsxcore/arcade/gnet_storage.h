/* G-Net hard-disk/PC Card/ATA composition layer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PCSX_ARCADE_GNET_STORAGE_H
#define PCSX_ARCADE_GNET_STORAGE_H

#include "gnet_pccard.h"
#include "rf5c296.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gnet_storage gnet_storage_t;

typedef enum gnet_storage_result {
  GNET_STORAGE_OK = 0,
  GNET_STORAGE_ARGUMENT = -1,
  GNET_STORAGE_NOMEM = -2,
  GNET_STORAGE_CHD = -3,
  GNET_STORAGE_METADATA = -4,
  GNET_STORAGE_GEOMETRY = -5,
  GNET_STORAGE_HASH_MISMATCH = -6,
  GNET_STORAGE_BUSY = -7,
  GNET_STORAGE_ATA = -8
} gnet_storage_result_t;

typedef struct gnet_storage_config {
  const char *chd_path;
  /* NULL means no writable overlay. CHD backing stays read-only either way. */
  const char *overlay_path;
  gnet_pccard_variant_t card_variant;
  /* Optional exact CHD SHA1, 20 bytes. The bytes are checked during open. */
  const uint8_t *expected_sha1;
  size_t expected_sha1_size;
  /* Defaults to MAME attotime::zero. */
  uint64_t dma_transfer_time_ns;
} gnet_storage_config_t;

typedef struct gnet_storage_info {
  uint32_t cylinders, heads, sectors_per_track, sector_count;
  uint8_t sha1[20];
  uint8_t metadata_cis_present;
  uint8_t metadata_key_present;
  uint8_t metadata_identify_present;
  size_t metadata_cis_bytes, metadata_key_bytes, metadata_identify_bytes;
} gnet_storage_info_t;

/* Opens the CHD read-only, checks optional SHA1 and ATA-compatible geometry,
 * loads raw CIS/KEY/IDNT, then constructs ATA HLE and the selected PC Card.
 * Missing CIS delegates to MAME's default-CIS behavior; present empty CIS is
 * kept distinct. Missing or non-five-byte KEY follows MAME's unlocked-card
 * behavior. A present 512-byte IDNT is used raw; another valid IDNT length is
 * retained in info but uses MAME's synthesized CF IDENTIFY builder. */
int gnet_storage_open(const gnet_storage_config_t *config,
                      gnet_storage_t **out, char *error,
                      size_t error_capacity);

/* Borrowed descriptor: gnet_zoom copies it during create. Its opaque state is
 * this object, which must outlive every RF5C296/G-Net board using the copy. */
const rf5c296_card_ops_t *gnet_storage_card_ops(gnet_storage_t *storage);
int gnet_storage_run_until(gnet_storage_t *storage, uint64_t monotonic_ns);
uint64_t gnet_storage_now(const gnet_storage_t *storage);
int gnet_storage_reset(gnet_storage_t *storage);
/* Persist writes only by this explicit call. */
int gnet_storage_flush(gnet_storage_t *storage);
void gnet_storage_get_info(const gnet_storage_t *storage,
                           gnet_storage_info_t *info);
int gnet_storage_is_locked(const gnet_storage_t *storage);
uint8_t gnet_storage_ata_status(const gnet_storage_t *storage);

/* Destroy the RF5C296/G-Net owner first so no copied callback descriptor can
 * be invoked. BUSY leaves this object and every child intact for a later
 * retry. Successful teardown detaches PC Card from ATA, then frees card, ATA,
 * and CHD in that order. */
int gnet_storage_try_destroy(gnet_storage_t *storage);
void gnet_storage_destroy(gnet_storage_t *storage);

#ifdef __cplusplus
}
#endif
#endif
