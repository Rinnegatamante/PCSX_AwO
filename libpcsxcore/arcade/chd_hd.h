/* Native CHD hard-disk backing, SPDX-License-Identifier: GPL-2.0-or-later. */
#ifndef PCSX_ARCADE_CHD_HD_H
#define PCSX_ARCADE_CHD_HD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct arcade_chd_hd arcade_chd_hd_t;
typedef struct arcade_chd_hd_info {
  uint32_t cylinders, heads, sectors_per_track, sector_bytes;
  uint32_t sector_count, hunk_bytes;
  uint8_t sha1[20];
  uint8_t inquiry[48];
  size_t inquiry_bytes;
} arcade_chd_hd_info_t;

typedef enum arcade_chd_hd_metadata_kind {
  ARCADE_CHD_HD_METADATA_GNET_CIS = 0,
  ARCADE_CHD_HD_METADATA_GNET_KEY = 1,
  ARCADE_CHD_HD_METADATA_ATA_IDENTIFY = 2
} arcade_chd_hd_metadata_kind_t;

typedef enum arcade_chd_hd_metadata_status {
  ARCADE_CHD_HD_METADATA_OK = 0,
  ARCADE_CHD_HD_METADATA_MISSING = 1,
  ARCADE_CHD_HD_METADATA_BUFFER_TOO_SMALL = 2,
  ARCADE_CHD_HD_METADATA_ERROR = -1,
  ARCADE_CHD_HD_METADATA_ARGUMENT = -2
} arcade_chd_hd_metadata_status_t;
/* overlay_path may be NULL for a read-only disk. A nonempty path enables
 * sector writes; a missing overlay starts empty. A malformed/wrong-image
 * overlay makes open fail. The source CHD is always opened read-only.
 * No callbacks, concurrency or implicit save on close; single-owner API.
 * Open errors are copied to error (when supplied), without printing paths. */
arcade_chd_hd_t *arcade_chd_hd_open(const char *path, const char *overlay_path,
                                  char *error, size_t error_bytes);
void arcade_chd_hd_close(arcade_chd_hd_t *disk);
void arcade_chd_hd_get_info(const arcade_chd_hd_t *disk, arcade_chd_hd_info_t *info);
/* Read raw CHD metadata: G-Net CIS  -> "CIS ", G-Net key -> "KEY ", and
 * ATA Identify block -> "IDNT". The IDNT metadata is returned in full; its
 * consumer must check for the expected 512 bytes. This does not alter the
 * legacy 48-byte SCSI inquiry field in arcade_chd_hd_info_t.
 *
 * On OK, *metadata_bytes is the exact length (including valid empty metadata).
 * On MISSING/ERROR/ARGUMENT, metadata_bytes and output remain unchanged. On
 * BUFFER_TOO_SMALL, output remains unchanged and metadata_bytes reports the
 * required capacity. A NULL output with capacity zero is a size query: it
 * returns OK for an empty value, otherwise BUFFER_TOO_SMALL. */
arcade_chd_hd_metadata_status_t arcade_chd_hd_read_metadata(
    const arcade_chd_hd_t *disk, arcade_chd_hd_metadata_kind_t kind,
    uint8_t *output, size_t capacity, size_t *metadata_bytes);
/* Zero success, -1 failure. A nscsi_hd callback adapter must return (result == 0).
 * read never modifies the caller buffer on failure. */
int arcade_chd_hd_read(void *disk, uint32_t lba, uint8_t *data, size_t bytes);
int arcade_chd_hd_write(void *disk, uint32_t lba, const uint8_t *data, size_t bytes);
/* Explicit atomic overlay save. Failure preserves an existing overlay.
 * A read-only disk returns success without filesystem writes. */
int arcade_chd_hd_flush(arcade_chd_hd_t *disk);
uint64_t arcade_chd_hd_modified_sectors(const arcade_chd_hd_t *disk);
const char *arcade_chd_hd_last_error(const arcade_chd_hd_t *disk);
#ifdef __cplusplus
}
#endif
#endif
