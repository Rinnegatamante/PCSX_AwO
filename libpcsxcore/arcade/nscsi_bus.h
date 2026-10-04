/* Standalone open-collector NSCSI bus resolver.
 * Derived from MAME src/devices/machine/nscsi_bus.cpp/.h.
 * MAME revision deb2f54fe7300e7a192690318beb860159bfd52a.
 * Copyright Olivier Galibert. SPDX-License-Identifier: BSD-3-Clause.
 */
#ifndef LIBPCSXCORE_ARCADE_NSCSI_BUS_H
#define LIBPCSXCORE_ARCADE_NSCSI_BUS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NscsiBus NscsiBus;
enum {
  NSCSI_INP = 0x001,
  NSCSI_CTL = 0x002,
  NSCSI_MSG = 0x004,
  NSCSI_BSY = 0x008,
  NSCSI_SEL = 0x010,
  NSCSI_REQ = 0x020,
  NSCSI_ACK = 0x040,
  NSCSI_ATN = 0x080,
  NSCSI_RST = 0x100,
  NSCSI_ALL = 0x1ff,
  NSCSI_PHASE_MASK = 7,
  NSCSI_PHASE_DATA_OUT = 0,
  NSCSI_PHASE_DATA_IN = 1,
  NSCSI_PHASE_COMMAND = 2,
  NSCSI_PHASE_STATUS = 3,
  NSCSI_PHASE_MSG_OUT = 6,
  NSCSI_PHASE_MSG_IN = 7
};
typedef struct nscsi_bus_ops {
  void *opaque;
  /* Receives aggregate levels excluding this endpoint's own outputs. */
  void (*changed)(void *opaque, uint32_t ctrl, uint32_t data);
} nscsi_bus_ops_t;
typedef struct nscsi_bus_status {
  uint64_t mutations, notifications;
  uint32_t faults;
} nscsi_bus_status_t;
enum {
  NSCSI_BUS_FAULT_QUEUE_OVERFLOW = 1u,
  NSCSI_BUS_FAULT_BAD_ENDPOINT = 2u,
  NSCSI_BUS_FAULT_REENTRANT_DESTROY = 4u
};
NscsiBus *nscsi_bus_create(void);
void nscsi_bus_destroy(NscsiBus *bus);
int nscsi_bus_attach(NscsiBus *bus, unsigned id, const nscsi_bus_ops_t *ops);
void nscsi_bus_detach(NscsiBus *bus, unsigned id);
void nscsi_bus_drive_ctrl(NscsiBus *bus, unsigned id, uint32_t lines,
                          uint32_t mask);
void nscsi_bus_drive_data(NscsiBus *bus, unsigned id, uint32_t data);
void nscsi_bus_wait_ctrl(NscsiBus *bus, unsigned id, uint32_t lines,
                         uint32_t mask);
uint32_t nscsi_bus_ctrl(const NscsiBus *bus);
uint32_t nscsi_bus_data(const NscsiBus *bus);
/* Aggregate levels excluding one endpoint, used by bus adapters whose chip
 * combines its own open-collector outputs internally. */
uint32_t nscsi_bus_ctrl_external(const NscsiBus *bus, unsigned id);
uint32_t nscsi_bus_data_external(const NscsiBus *bus, unsigned id);
/* Deliver queued per-mutation snapshots in order. Call after each wire
 * mutation; callbacks may enqueue further changes, drained before return. */
int nscsi_bus_flush(NscsiBus *bus);
void nscsi_bus_get_status(const NscsiBus *bus, nscsi_bus_status_t *status);
#ifdef __cplusplus
}
#endif
#endif
