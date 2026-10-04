/* license:BSD-3-Clause
 * copyright-holders:R. Belmont, smf
 * Based on MAME src/devices/machine/mb89371.cpp and machine/clock.cpp at
 * deb2f54fe7300e7a192690318beb860159bfd52a.
 */
#include "mb89371.h"
#include <stdlib.h>
#include <string.h>

enum { USE_BRG = 1, LOOPBACK = 3, MASK_TXREADY = 5, MASK_RXREADY = 6 };
typedef struct mb_channel_context {
  struct mb89371 *device;
  unsigned channel;
} mb_channel_context_t;
typedef struct brg {
  uint64_t next_ns, fraction;
  uint32_t rate;
  int active, level;
} brg_t;
struct mb89371 {
  uint32_t master_hz;
  mb89371_callbacks_t cb;
  i8251_t *sio[2];
  mb_channel_context_t channel_context[2];
  uint8_t mode[2], baud[2];
  brg_t brg[2];
  uint64_t now_ns;
  uint32_t faults;
  int busy;
};
static int valid(const mb89371_t *d, unsigned ch) { return d && ch < 2; }
static void relay(mb89371_t *d, unsigned ch, i8251_line_cb cb, int v) {
  (void)ch;
  if (cb) {
    d->busy = 1;
    cb(d->cb.user, !!v);
    d->busy = 0;
  }
}
static void txd_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  unsigned ch = context->channel;
  if (d->mode[ch] & (1 << LOOPBACK))
    i8251_loopback_rxd(d->sio[ch], v);
  else
    relay(d, ch, d->cb.txd[ch], v);
}
static void dtr_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  relay(d, context->channel, d->cb.dtr[context->channel], v);
}
static void rts_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  relay(d, context->channel, d->cb.rts[context->channel], v);
}
static void rxready_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  unsigned ch = context->channel;
  if (d->mode[ch] & (1 << MASK_RXREADY))
    relay(d, ch, d->cb.rxready[ch], v);
}
static void txready_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  unsigned ch = context->channel;
  if (d->mode[ch] & (1 << MASK_TXREADY))
    relay(d, ch, d->cb.txready[ch], v);
}
/* MAME source quirk: TXEMPTY and SYNDET use the TXREADY mask bit. */
static void txempty_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  unsigned ch = context->channel;
  if (d->mode[ch] & (1 << MASK_TXREADY))
    relay(d, ch, d->cb.txempty[ch], v);
}
static void syndet_callback(void *p, int v) {
  mb_channel_context_t *context = p;
  mb89371_t *d = context->device;
  unsigned ch = context->channel;
  if (d->mode[ch] & (1 << MASK_TXREADY))
    relay(d, ch, d->cb.syndet[ch], v);
}
static i8251_callbacks_t child_cb(mb89371_t *d, unsigned ch) {
  i8251_callbacks_t c;
  memset(&c, 0, sizeof(c));
  c.user = &d->channel_context[ch];
  c.txd = txd_callback;
  c.dtr = dtr_callback;
  c.rts = rts_callback;
  c.rxrdy = rxready_callback;
  c.txrdy = txready_callback;
  c.txempty = txempty_callback;
  c.syndet = syndet_callback;
  return c;
}
static uint32_t baud_rate(mb89371_t *d, unsigned ch) {
  unsigned shift = d->baud[ch];
  if (shift >= 30) {
    d->faults |= MB89371_FAULT_INVALID_BAUD;
    return 0;
  }
  uint32_t div = 2u << shift;
  uint32_t rate = d->master_hz / div;
  if (!rate && d->master_hz)
    d->faults |= MB89371_FAULT_ZERO_RATE;
  return rate;
}
static void schedule_period(mb89371_t *d, unsigned ch) {
  brg_t *b = &d->brg[ch];
  uint32_t rate;
  if (!(d->mode[ch] & (1 << USE_BRG)) || !(rate = baud_rate(d, ch))) {
    b->active = 0;
    b->rate = 0;
    return;
  }
  uint64_t den = (uint64_t)rate * 2;
  uint64_t rem =
      b->active && b->next_ns > d->now_ns ? b->next_ns - d->now_ns : 0;
  /* clock_device::reinit only shortens an already pending edge. */
  if (den > 1000000000ULL)
    d->faults |= MB89371_FAULT_SUBNANOSECOND_CLOCK;
  uint64_t half = (1000000000ULL + den - 1) / den;
  if (!half) {
    half = 1;
    d->faults |= MB89371_FAULT_SUBNANOSECOND_CLOCK;
  }
  b->rate = rate;
  /* Keep phase just after the first scheduled edge, so later delays yield
   * cumulative times ceil(k * 1e9 / (2 * rate)). */
  b->fraction = (den - 1 + 1000000000ULL) % den;
  b->active = 1;
  if (!rem || half < rem) {
    if (UINT64_MAX - d->now_ns < half) {
      b->active = 0;
      d->faults |= MB89371_FAULT_TIME_OVERFLOW;
      return;
    }
    b->next_ns = d->now_ns + half;
  }
}
mb89371_t *mb89371_create(uint32_t hz, const mb89371_callbacks_t *cb) {
  if (!hz)
    return NULL;
  mb89371_t *d = (mb89371_t *)calloc(1, sizeof(*d));
  if (!d)
    return NULL;
  d->master_hz = hz;
  if (cb)
    d->cb = *cb;
  d->mode[0] = d->mode[1] = 0xf0;
  for (unsigned ch = 0; ch < 2; ch++) {
    d->channel_context[ch].device = d;
    d->channel_context[ch].channel = ch;
    i8251_callbacks_t c = child_cb(d, ch);
    d->sio[ch] = i8251_create(&c);
    if (!d->sio[ch]) {
      mb89371_destroy(d);
      return NULL;
    }
  }
  return d;
}
void mb89371_destroy(mb89371_t *d) {
  if (!d || d->busy)
    return;
  i8251_destroy(d->sio[0]);
  i8251_destroy(d->sio[1]);
  free(d);
}
void mb89371_reset(mb89371_t *d) {
  if (!d || d->busy)
    return;
  i8251_reset(d->sio[0]);
  i8251_reset(d->sio[1]);
}
uint8_t mb89371_read(mb89371_t *d, unsigned ch, unsigned off) {
  if (!valid(d, ch) || d->busy)
    return 0xff;
  switch (off) {
  case 0:
    return i8251_read(d->sio[ch], 0);
  case 1:
    return i8251_read(d->sio[ch], 1);
  case 2:
    return d->baud[ch];
  case 3:
    return d->mode[ch];
  default:
    return 0xff;
  }
}
void mb89371_write(mb89371_t *d, unsigned ch, unsigned off, uint8_t v) {
  if (!valid(d, ch) || d->busy)
    return;
  switch (off) {
  case 0:
    i8251_write(d->sio[ch], 0, v);
    break;
  case 1:
    i8251_write(d->sio[ch], 1, v);
    break;
  case 2:
    d->baud[ch] = v;
    if (v >= 30)
      d->faults |= MB89371_FAULT_INVALID_BAUD;
    schedule_period(d, ch);
    break;
  case 3:
    d->mode[ch] = v;
    schedule_period(d, ch);
    break;
  default:
    break;
  }
}
void mb89371_write_rxd(mb89371_t *d, unsigned ch, int v) {
  if (valid(d, ch) && !d->busy && !(d->mode[ch] & (1 << LOOPBACK)))
    i8251_write_rxd(d->sio[ch], v);
}
void mb89371_write_cts(mb89371_t *d, unsigned ch, int v) {
  if (valid(d, ch) && !d->busy)
    i8251_write_cts(d->sio[ch], v);
}
void mb89371_write_dsr(mb89371_t *d, unsigned ch, int v) {
  if (valid(d, ch) && !d->busy)
    i8251_write_dsr(d->sio[ch], v);
}
void mb89371_write_txc(mb89371_t *d, unsigned ch, int v) {
  if (valid(d, ch) && !d->busy && !(d->mode[ch] & (1 << USE_BRG)))
    i8251_write_txc(d->sio[ch], v);
}
void mb89371_write_rxc(mb89371_t *d, unsigned ch, int v) {
  if (valid(d, ch) && !d->busy && !(d->mode[ch] & (1 << USE_BRG)))
    i8251_write_rxc(d->sio[ch], v);
}
int mb89371_txrdy(const mb89371_t *d, unsigned ch) {
  return valid(d, ch) ? i8251_txrdy(d->sio[ch]) : 0;
}
int mb89371_rxrdy(const mb89371_t *d, unsigned ch) {
  return valid(d, ch) ? i8251_rxrdy(d->sio[ch]) : 0;
}
uint64_t mb89371_time_ns(const mb89371_t *d) { return d ? d->now_ns : 0; }
int mb89371_next_event_ns(const mb89371_t *d, uint64_t *next) {
  int found = 0;
  uint64_t due = UINT64_MAX;
  if (d) for (unsigned i = 0; i < 2; ++i) {
    if (d->brg[i].active && (!found || d->brg[i].next_ns < due)) {
      due = d->brg[i].next_ns;
      found = 1;
    }
  }
  if (next) *next = due;
  return found;
}
uint32_t mb89371_faults(const mb89371_t *d) { return d ? d->faults : 0; }
void mb89371_advance(mb89371_t *d, uint64_t now) {
  if (!d || d->busy)
    return;
  if (now < d->now_ns) {
    d->faults |= MB89371_FAULT_TIME_REWIND;
    return;
  }
  unsigned events = 0;
  while (1) {
    int ch = -1;
    uint64_t due = UINT64_MAX;
    for (unsigned i = 0; i < 2; i++)
      if (d->brg[i].active && d->brg[i].next_ns <= now &&
          (ch < 0 || d->brg[i].next_ns < due)) {
        ch = (int)i;
        due = d->brg[i].next_ns;
      }
    if (ch < 0)
      break;
    if (events == 1000000u) {
      d->faults |= MB89371_FAULT_EVENT_BUDGET;
      return;
    }
    events++;
    brg_t *b = &d->brg[ch];
    d->now_ns = due;
    b->level = !b->level;
    i8251_write_txc(d->sio[ch], b->level);
    i8251_write_rxc(d->sio[ch], b->level);
    uint64_t den = (uint64_t)b->rate * 2;
    b->fraction += 1000000000ULL;
    uint64_t step = b->fraction / den;
    b->fraction %= den;
    if (!step) {
      step = 1;
      b->fraction = 0;
      d->faults |= MB89371_FAULT_SUBNANOSECOND_CLOCK;
    }
    if (UINT64_MAX - b->next_ns < step) {
      b->active = 0;
      d->faults |= MB89371_FAULT_TIME_OVERFLOW;
      continue;
    }
    b->next_ns += step;
  }
  d->now_ns = now;
}
