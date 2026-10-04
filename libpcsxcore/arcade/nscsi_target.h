/* MAME-derived nscsi_full_device target-side protocol engine.
 * MAME revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright Olivier Galibert. SPDX-License-Identifier: BSD-3-Clause.
 */
#ifndef LIBPCSXCORE_ARCADE_NSCSI_TARGET_H
#define LIBPCSXCORE_ARCADE_NSCSI_TARGET_H
#include "nscsi_bus.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NscsiTarget NscsiTarget;
enum {
  NSCSI_STATUS_GOOD = 0x00,
  NSCSI_STATUS_CHECK_CONDITION = 0x02,
  NSCSI_MESSAGE_COMMAND_COMPLETE = 0x00
};
typedef struct nscsi_target_backend {
  void *opaque;
  /* Called once after upstream CDB group-length recognition. Callback must
   * queue phases only; it must not re-enter bus flushing or target advance. */
  void (*command)(void *opaque, NscsiTarget *target, const uint8_t *cdb,
                  size_t length);
  uint8_t (*read_data)(void *opaque, unsigned buffer, size_t offset);
  void (*write_data)(void *opaque, unsigned buffer, size_t offset,
                     uint8_t value);
  /* Optional source-specific CDB recognition: -1 uses the default group
   * rule, 0 awaits another byte, 1 completes. Pure query; no phase mutation.
   * Used by the CD base's 10-byte vendor-command override. */
  int (*command_done)(void *opaque, uint8_t opcode, size_t length);
} nscsi_target_backend_t;
typedef struct nscsi_target_status {
  uint64_t now_ns, timer_deadline_ns, last_timer_fire_ns;
  uint32_t faults;
  unsigned scsi_id, initiator_id;
  uint16_t cdb_length;
  uint8_t state, identify;
} nscsi_target_status_t;
enum {
  NSCSI_TARGET_FAULT_TIME_REWIND = 1u,
  NSCSI_TARGET_FAULT_TIME_OVERFLOW = 2u,
  NSCSI_TARGET_FAULT_QUEUE = 4u,
  NSCSI_TARGET_FAULT_CDB_OVERFLOW = 8u,
  NSCSI_TARGET_FAULT_BACKEND = 16u,
  NSCSI_TARGET_FAULT_UNSUPPORTED_MESSAGE = 32u,
  NSCSI_TARGET_FAULT_REENTRANT = 64u,
  NSCSI_TARGET_FAULT_PHASE_BACKEND = 128u
};
NscsiTarget *nscsi_target_create(NscsiBus *bus, unsigned scsi_id,
                                 const nscsi_target_backend_t *backend);
void nscsi_target_destroy(NscsiTarget *target);
void nscsi_target_reset(NscsiTarget *target);
/* Absolute monotonic integer nanoseconds. Target defaults follow MAME: 400ns
 * selection settle; byte period and command delay are zero. */
void nscsi_target_advance_to(NscsiTarget *target, uint64_t now_ns);
int nscsi_target_data_in(NscsiTarget *target, unsigned buffer, size_t size);
int nscsi_target_data_out(NscsiTarget *target, unsigned buffer, size_t size);
int nscsi_target_status_complete(NscsiTarget *target, uint8_t status);
int nscsi_target_message_in(NscsiTarget *target, uint8_t message);
int nscsi_target_bus_free(NscsiTarget *target);
void nscsi_target_get_status(const NscsiTarget *target,
                             nscsi_target_status_t *status);
#ifdef __cplusplus
}
#endif
#endif
