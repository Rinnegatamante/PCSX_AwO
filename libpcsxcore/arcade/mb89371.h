/* license:BSD-3-Clause
 * copyright-holders:R. Belmont, smf
 * Standalone C port of MAME's Fujitsu MB89371 dual UART.
 */
#ifndef PCSXR_ARCADE_MB89371_H
#define PCSXR_ARCADE_MB89371_H
#include "i8251.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mb89371 mb89371_t;
typedef struct mb89371_callbacks {
  void *user;
  i8251_line_cb txd[2], dtr[2], rts[2], rxready[2], txready[2], txempty[2],
      syndet[2];
} mb89371_callbacks_t;
enum {
  MB89371_FAULT_INVALID_BAUD = 1u << 0,
  MB89371_FAULT_TIME_REWIND = 1u << 1,
  MB89371_FAULT_TIME_OVERFLOW = 1u << 2,
  MB89371_FAULT_SUBNANOSECOND_CLOCK = 1u << 3,
  MB89371_FAULT_EVENT_BUDGET = 1u << 4,
  MB89371_FAULT_ZERO_RATE = 1u << 5
};
mb89371_t *mb89371_create(uint32_t master_clock_hz,
                          const mb89371_callbacks_t *callbacks);
void mb89371_destroy(mb89371_t *device);
void mb89371_reset(mb89371_t *device);
uint8_t mb89371_read(mb89371_t *device, unsigned channel, unsigned offset);
void mb89371_write(mb89371_t *device, unsigned channel, unsigned offset,
                   uint8_t data);
void mb89371_write_rxd(mb89371_t *device, unsigned channel, int level);
void mb89371_write_cts(mb89371_t *device, unsigned channel, int level);
void mb89371_write_dsr(mb89371_t *device, unsigned channel, int level);
void mb89371_write_txc(mb89371_t *device, unsigned channel, int level);
void mb89371_write_rxc(mb89371_t *device, unsigned channel, int level);
int mb89371_txrdy(const mb89371_t *device, unsigned channel);
int mb89371_rxrdy(const mb89371_t *device, unsigned channel);
/* Advance monotonic virtual nanoseconds. BRG edges use integer-Hz
 * floor(master/(2<<baud)); query the timestamp during callbacks with
 * mb89371_time_ns(). */
void mb89371_advance(mb89371_t *device, uint64_t now_ns);
uint64_t mb89371_time_ns(const mb89371_t *device);
/* Scheduler query between advances: returns 1 and writes the earliest active
 * BRG edge, or 0 when none is active. UINT64_MAX can be a valid edge, so it
 * must not be used as the sole no-event sentinel. Does not change state. */
int mb89371_next_event_ns(const mb89371_t *device, uint64_t *next_ns);
uint32_t mb89371_faults(const mb89371_t *device);
#ifdef __cplusplus
}
#endif
#endif
