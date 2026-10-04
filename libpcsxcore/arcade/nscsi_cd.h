/* MAME-derived common NSCSI CD-ROM target and Toshiba XM-5401 profile.
 * Source revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * BSD-3-Clause; see nscsi_cd_LICENSE.txt.
 */
#ifndef LIBPCSXCORE_ARCADE_NSCSI_CD_H
#define LIBPCSXCORE_ARCADE_NSCSI_CD_H
#include "nscsi_bus.h"
#include "nscsi_target.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NSCSI_CD_SECTOR_BYTES = 2048, NSCSI_CD_MAX_BUFFER = 65536 };
typedef struct NscsiCdrom NscsiCdrom;

typedef struct nscsi_cd_audio_status {
  int active;
  int paused;
  int ended;
  uint32_t lba;
} nscsi_cd_audio_status_t;

/* Every operation is supplied by the caller's real local disc/media backend.
 * Track indexes mirror MAME cdrom_file: zero-based tracks; 0xaa is lead-out
 * for track_start/ADR-control. read_mode1 returns nonzero on successful data.
 */
typedef struct nscsi_cd_media_ops {
  void *opaque;
  int (*present)(void *opaque);
  uint64_t (*sequence_counter)(void *opaque);
  int (*read_mode1_sector)(void *opaque, uint32_t lba,
                           uint8_t out[NSCSI_CD_SECTOR_BYTES]);
  uint32_t (*track_start)(void *opaque, unsigned track_or_leadout);
  unsigned (*last_track)(void *opaque);
  unsigned (*track_for_lba)(void *opaque, uint32_t lba);
  uint8_t (*track_adr_control)(void *opaque, unsigned track_or_leadout);
  int (*track_is_audio)(void *opaque, unsigned track_index);
  void (*cdda_start)(void *opaque, uint32_t lba, uint32_t blocks);
  void (*cdda_pause)(void *opaque, int pause);
  void (*cdda_status)(void *opaque, nscsi_cd_audio_status_t *status);
  void (*cdda_set_gain)(void *opaque, unsigned channel, float gain);
} nscsi_cd_media_ops_t;

typedef struct nscsi_cdrom_config {
  NscsiBus *bus;
  unsigned scsi_id;
  /* MAME defaults to 2048; the Sony/Sun/SGI source configurations set 512.
   * XM-5401 profiles use 2048 unless the board explicitly overrides it. */
  uint32_t block_size;
  nscsi_cd_media_ops_t media;
} nscsi_cdrom_config_t;

typedef struct nscsi_cdrom_status {
  uint32_t block_size;
  uint32_t faults;
  uint64_t media_sequence;
  uint8_t sense_key, asc, ascq;
  uint8_t removal_prevented, cdda_sotc;
  uint8_t manufacturer[8], product[16], revision[4];
  uint8_t inquiry_data, compliance;
  nscsi_target_status_t target;
} nscsi_cdrom_status_t;

enum {
  NSCSI_CD_FAULT_BAD_MEDIA_READ = 1u,
  NSCSI_CD_FAULT_UNSUPPORTED_TOOLBOX = 2u,
  NSCSI_CD_FAULT_REENTRANT = 4u,
  NSCSI_CD_FAULT_BACKEND = 8u,
  NSCSI_CD_FAULT_MALFORMED_MODE = 16u
};

/* Creates the generic CD-ROM HLE endpoint with XM-5401 inquiry identity:
 * TOSHIBA / CD-ROM XM-5401TA / 3605 / inquiry 0x98 / compliance 0x02. */
NscsiCdrom *nscsi_cdrom_create(const nscsi_cdrom_config_t *config);
void nscsi_cdrom_destroy(NscsiCdrom *cd);
void nscsi_cdrom_reset(NscsiCdrom *cd);
void nscsi_cdrom_advance_to(NscsiCdrom *cd, uint64_t now_ns);
void nscsi_cdrom_get_status(const NscsiCdrom *cd, nscsi_cdrom_status_t *status);
#ifdef __cplusplus
}
#endif
#endif
