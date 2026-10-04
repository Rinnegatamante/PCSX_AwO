/* Read-only CHD CD backend, using the repository-pinned libchdr. */
#ifndef LIBPCSXCORE_ARCADE_CHD_CD_H
#define LIBPCSXCORE_ARCADE_CHD_CD_H
#include "nscsi_cd.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct ChdCd ChdCd;
typedef void (*chd_cd_pcm_sink_t)(void *opaque, int16_t left, int16_t right);
typedef struct chd_cd_config {
  chd_cd_pcm_sink_t pcm_sink;
  void *pcm_opaque;
} chd_cd_config_t;

/* Opens a real CHD containing supported CD track metadata. Failure is closed:
 * no virtual disc, track, zero sector, or ready state is supplied. */
ChdCd *chd_cd_open(const char *path, const chd_cd_config_t *config,
                   char *error, size_t error_capacity);
void chd_cd_close(ChdCd *cd);
int chd_cd_get_media_ops(ChdCd *cd, nscsi_cd_media_ops_t *ops);
/* Advance absolute nanoseconds. Time must be monotonic. CDDA emits 44.1 kHz,
 * stereo signed PCM16 samples from the actual audio sectors to the sink. */
int chd_cd_advance_to(ChdCd *cd, uint64_t now_ns);
uint32_t chd_cd_last_lba(const ChdCd *cd);
uint32_t chd_cd_track_count(const ChdCd *cd);
const char *chd_cd_last_error(const ChdCd *cd);
#ifdef __cplusplus
}
#endif
#endif
