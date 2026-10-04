/* license:BSD-3-Clause
 * copyright-holders:smf, Robbbert
 * Standalone C port based on MAME src/devices/machine/i8251.cpp and
 * src/emu/diserial.cpp at deb2f54fe7300e7a192690318beb860159bfd52a.
 */
#include "i8251.h"
#include <stdlib.h>
#include <string.h>

enum {
  FE = 0x20,
  OE = 0x10,
  PE = 0x08,
  TXEMPTY = 0x04,
  RXREADY = 0x02,
  TXREADY = 0x01
};
enum { NEXT_COMMAND, NEXT_MODE, NEXT_SYNC1, NEXT_SYNC2 };
struct i8251 {
  i8251_callbacks_t cb;
  uint8_t flags, sync_bytes, status, command, mode, delayed_tx;
  uint8_t rx_data, tx_data, sync8, tx_sync_shift, rxd_bits, data_bits;
  uint16_t sync16;
  uint8_t tx_bits[12], tx_count, tx_sent;
  uint8_t rx_active, rx_count, rx_shift, rx_parity, rx_frame_errors;
  uint8_t rx_stop, tx_stop, start_bits, parity_enable, parity_even;
  uint16_t rx_history;
  int tx_reg_empty;
  int cts, dsr, rxd, rxc, txc, rxc_count, txc_count, br_factor;
  int syndet_pin, hunt_on, ext_syn_set, busy;
  uint32_t sync_accum, sync_accum2;
};

static void out(i8251_t *u, i8251_line_cb cb, int level) {
  if (cb) {
    u->busy = 1;
    cb(u->cb.user, !!level);
    u->busy = 0;
  }
}
static void rx_update(i8251_t *u) {
  out(u, u->cb.rxrdy, (u->command & 4) && (u->status & RXREADY));
}
static int txenabled(const i8251_t *u) { return (u->command & 1) && !u->cts; }
int i8251_txrdy(const i8251_t *u) {
  return u && txenabled(u) && (u->status & TXREADY);
}
int i8251_rxrdy(const i8251_t *u) {
  return u && (u->command & 4) && (u->status & RXREADY);
}
static void txready_update(i8251_t *u) { out(u, u->cb.txrdy, i8251_txrdy(u)); }
static void txempty_update(i8251_t *u) {
  out(u, u->cb.txempty, (u->status & TXEMPTY) != 0);
}
static void syndet_update(i8251_t *u, int level) {
  if (level)
    u->status |= 0x40;
  else
    u->status &= (uint8_t)~0x40;
  if (!u->syndet_pin)
    out(u, u->cb.syndet, level);
}
static void setup_tx(i8251_t *u, uint8_t data) {
  unsigned i, n = 0;
  for (i = 0; i < u->start_bits; i++)
    u->tx_bits[n++] = 0;
  for (i = 0; i < u->data_bits; i++)
    u->tx_bits[n++] = (data >> i) & 1;
  if (u->parity_enable) {
    unsigned ones = 0;
    for (i = 0; i < u->data_bits; i++)
      ones += (data >> i) & 1;
    u->tx_bits[n++] = ((ones & 1) ^ (u->parity_even ? 0 : 1));
  }
  for (i = 0; i < u->tx_stop; i++)
    u->tx_bits[n++] = 1;
  u->tx_count = (uint8_t)n;
  u->tx_sent = 0;
  u->tx_reg_empty = 0;
  u->status &= (uint8_t)~TXEMPTY;
}
static void maybe_start_tx(i8251_t *u) {
  if (txenabled(u) && !(u->status & TXREADY) && (u->status & TXEMPTY)) {
    setup_tx(u, u->tx_data);
    u->status |= TXREADY;
  }
}
static void tx_idle(i8251_t *u) {
  if (u->status & TXEMPTY) {
    if (u->sync_bytes && txenabled(u)) {
      if (u->sync_bytes == 2) {
        setup_tx(u, (uint8_t)(u->sync16 >> u->tx_sync_shift));
        u->tx_sync_shift ^= 8;
      } else
        setup_tx(u, u->sync8);
    } else
      out(u, u->cb.txd, !(u->command & 8));
  }
  txempty_update(u);
}
static void tx_start_check(i8251_t *u) { maybe_start_tx(u); }

static void receive_char(i8251_t *u, uint8_t c) {
  u->rx_data = c;
  if (u->status & RXREADY)
    u->status |= OE;
  u->status |= RXREADY;
  rx_update(u);
}
static unsigned pop8(uint8_t x) {
  unsigned n = 0;
  for (; x; x &= (uint8_t)(x - 1))
    n++;
  return n;
}
static void async_sample(i8251_t *u, int bit) {
  int previous = !!(u->rx_history & 0x8000);
  u->rx_history = (uint16_t)((u->rx_history >> 1) | ((bit & 1) << 15));
  /* Shift-register behavior follows device_serial_interface: start is an
     observed high/low transition; following samples fill MSB-first. */
  if (!u->rx_active) {
    if (previous != bit && !bit) {
      u->rx_active = 1;
      u->rx_count = 0;
      u->rx_shift = 0;
      u->rx_parity = 0;
      u->rx_frame_errors = 0;
    }
    return;
  }
  u->rx_count++;
  if (u->rx_count <= u->data_bits) {
    u->rx_shift |= (uint16_t)(bit << (u->rx_count - 1));
    u->rx_parity ^= (uint8_t)bit;
  } else if (u->parity_enable && u->rx_count == (unsigned)u->data_bits + 1) {
    int expected = u->parity_even ? u->rx_parity : !u->rx_parity;
    if (bit != expected)
      u->rx_frame_errors |= PE;
  } else if (u->rx_count >
             (unsigned)u->data_bits + (u->parity_enable ? 1 : 0)) {
    if (!bit)
      u->rx_frame_errors |= FE;
  }
  if (u->rx_count >=
      (unsigned)u->data_bits + (u->parity_enable ? 1 : 0) + u->rx_stop) {
    u->status |= u->rx_frame_errors;
    receive_char(u, (uint8_t)u->rx_shift);
    u->rx_active = 0;
    u->rx_count = 0;
  }
}
static void sync_sample(i8251_t *u, int bit) {
  unsigned parity = !!(u->mode & 0x10), n = u->data_bits;
  int matched_hunt = 0;
  if (u->syndet_pin && !u->ext_syn_set)
    return;
  if (parity && u->rxd_bits == n) {
    if (((pop8((uint8_t)u->sync_accum) + bit + ((u->mode >> 5) & 1)) & 1) == 0)
      u->status |= PE;
  } else {
    u->sync_accum = (u->sync_accum >> 1) | ((uint32_t)bit << (n - 1));
    if (u->sync_bytes == 2)
      u->sync_accum2 = (u->sync_accum2 >> 1) | ((uint32_t)bit << (n * 2 - 1));
  }
  if (u->hunt_on) {
    uint32_t expect = u->sync_bytes == 2 ? u->sync16 : u->sync8;
    if ((u->sync_bytes == 2 ? u->sync_accum2 : u->sync_accum) != expect)
      return;
    if (parity && u->rxd_bits < n) {
      u->rxd_bits = (uint8_t)n;
      return;
    }
    u->rxd_bits = (uint8_t)n;
    u->hunt_on = 0;
    matched_hunt = 1;
  }
  u->rxd_bits++;
  if (u->rxd_bits < n + parity)
    return;
  if (!matched_hunt) {
    receive_char(u, (uint8_t)u->sync_accum);
  }
  if ((u->sync_bytes == 2 && u->sync_accum2 == u->sync16) ||
      (u->sync_bytes == 1 && u->sync_accum == u->sync8))
    syndet_update(u, 1);
  u->rxd_bits = 0;
  u->sync_accum = 0;
  u->sync_accum2 = 0;
}

i8251_t *i8251_create(const i8251_callbacks_t *cb) {
  i8251_t *u = (i8251_t *)calloc(1, sizeof(*u));
  if (!u)
    return NULL;
  if (cb)
    u->cb = *cb;
  u->cts = 1;
  u->dsr = 1;
  u->rxd = 1;
  u->rx_history = 0x8000;
  u->tx_reg_empty = 1;
  i8251_reset(u);
  return u;
}
void i8251_destroy(i8251_t *u) {
  if (u && !u->busy)
    free(u);
}
void i8251_reset(i8251_t *u) {
  if (!u || u->busy)
    return;
  /* Match device_reset: serial frame configuration, shift history and
     synchronous programming state survive; generic register reset only
     marks TX empty and clears RX progress/full state. */
  out(u, u->cb.txd, 1);
  out(u, u->cb.rts, 1);
  out(u, u->cb.dtr, 1);
  u->flags = NEXT_MODE;
  u->status = TXEMPTY | TXREADY;
  u->command = u->mode = 0;
  u->rx_data = u->tx_data = 0;
  u->rxc_count = u->txc_count = 0;
  u->br_factor = 1;
  u->rx_active = 0;
  u->rx_count = 0;
  u->tx_reg_empty = 1;
  tx_idle(u);
  rx_update(u);
  txready_update(u);
}
uint8_t i8251_read(i8251_t *u, unsigned offset) {
  uint8_t value;
  if (!u || u->busy)
    return 0xff;
  if (offset == 0) {
    value = u->rx_data;
    u->status &= (uint8_t)~RXREADY;
    rx_update(u);
    return value;
  }
  value = (uint8_t)((u->dsr << 7) | u->status);
  syndet_update(u, 0);
  return value;
}
static void mode_write(i8251_t *u, uint8_t d) {
  unsigned stops = (d >> 6) & 3;
  u->mode = d;
  u->data_bits = (uint8_t)(((d >> 2) & 3) + 5);
  u->br_factor = 1;
  u->parity_enable = !!(d & 0x10);
  u->parity_even = !!(d & 0x20);
  u->start_bits = 0;
  u->tx_stop = 0;
  u->rx_stop = 0;
  u->flags = NEXT_COMMAND;
  u->sync_bytes = 0;
  u->syndet_pin = 0;
  if (d & 3) {
    u->start_bits = 1;
    u->br_factor = (d & 3) == 2 ? 16 : (d & 3) == 3 ? 64 : 1;
    u->tx_stop = stops == 1 ? 1 : stops == 0 ? 0 : 2;
    u->rx_stop = stops ? 1 : 0;
  } else {
    u->flags = (d & 0x80) ? NEXT_SYNC2 : NEXT_SYNC1;
    u->sync_bytes = (d & 0x80) ? 1 : 2;
    u->syndet_pin = (d >> 6) & 1;
    u->sync8 = 0;
    u->sync16 = 0;
    u->tx_sync_shift = 8;
    u->rxd_bits = 0;
    u->sync_accum = 0; /* MAME resets sync1 but leaves sync2 intact. */
  }
  u->rx_active = 0;
  u->rx_count = 0;
  u->txc_count = 0;
}
static void command_write(i8251_t *u, uint8_t d) {
  u->command = d;
  out(u, u->cb.rts, !(d & 0x20));
  out(u, u->cb.dtr, !(d & 2));
  if (d & 0x10)
    u->status &= (uint8_t)~(PE | OE | FE);
  if (d & 0x40)
    u->flags = NEXT_MODE;
  u->hunt_on = 0;
  syndet_update(u, 0);
  if (u->sync_bytes)
    u->hunt_on = !!(d & 0x80);
  if (u->hunt_on)
    u->ext_syn_set = 0;
  if (d & 8)
    out(u, u->cb.txd, 0);
  tx_start_check(u);
  rx_update(u);
  txready_update(u);
  tx_idle(u);
}
void i8251_write(i8251_t *u, unsigned offset, uint8_t d) {
  if (!u || u->busy)
    return;
  if (offset == 0) {
    u->tx_data = d;
    u->status &= (uint8_t)~TXREADY;
    txready_update(u);
    u->delayed_tx = (uint8_t)txenabled(u);
    tx_start_check(u);
    txready_update(u);
    tx_idle(u);
    return;
  }
  if (u->flags == NEXT_SYNC1) {
    u->sync16 = (uint16_t)d << 8;
    u->flags = NEXT_SYNC2;
  } else if (u->flags == NEXT_SYNC2) {
    if (u->sync_bytes == 2)
      u->sync16 |= d;
    else
      u->sync8 = d;
    u->flags = NEXT_COMMAND;
  } else if (u->flags == NEXT_MODE)
    mode_write(u, d);
  else
    command_write(u, d);
}
void i8251_write_rxd(i8251_t *u, int level) {
  if (u && !u->busy)
    u->rxd = !!level;
}
void i8251_loopback_rxd(i8251_t *u, int level) {
  if (u)
    u->rxd = !!level;
}
void i8251_write_cts(i8251_t *u, int level) {
  if (!u || u->busy)
    return;
  u->cts = !!level;
  tx_start_check(u);
  txready_update(u);
  tx_idle(u);
}
void i8251_write_dsr(i8251_t *u, int level) {
  if (u && !u->busy)
    u->dsr = !level;
}
void i8251_write_syn(i8251_t *u, int level) {
  if (u && !u->busy && u->syndet_pin && level) {
    u->ext_syn_set = 1;
    u->hunt_on = 0;
    syndet_update(u, 1);
  }
}
void i8251_write_rxc(i8251_t *u, int level) {
  if (!u || u->busy)
    return;
  level = !!level;
  if (!u->rxc && level && (u->command & 4)) {
    if (u->sync_bytes)
      sync_sample(u, u->rxd);
    else {
      if (u->rx_active) {
        if (--u->rxc_count == 0) {
          async_sample(u, u->rxd);
          u->rxc_count = u->rx_active ? u->br_factor : 0;
        }
      } else {
        async_sample(u, u->rxd);
        if (u->rx_active)
          u->rxc_count = (3 * u->br_factor) / 2;
      }
    }
  }
  u->rxc = level;
}
void i8251_write_txc(i8251_t *u, int level) {
  if (!u || u->busy)
    return;
  level = !!level;
  if (u->txc && !level) {
    if (++u->txc_count == u->br_factor) {
      u->txc_count = 0;
      if (u->tx_reg_empty) {
        if (!(u->status & TXREADY) && (txenabled(u) || u->delayed_tx)) {
          setup_tx(u, u->tx_data);
          u->status |= TXREADY;
        } else
          u->status |= TXEMPTY;
        txready_update(u);
        tx_idle(u);
      }
      if (!u->tx_reg_empty && u->tx_sent < u->tx_count) {
        out(u, u->cb.txd, u->tx_bits[u->tx_sent++]);
        if (u->tx_sent == u->tx_count)
          u->tx_reg_empty = 1;
      }
    }
  }
  u->txc = level;
}
