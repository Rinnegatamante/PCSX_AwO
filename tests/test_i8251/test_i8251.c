#include "../../libpcsxcore/arcade/i8251.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct trace {
  i8251_t *u;
  int tx[256], n, rxready, txready, empty, syn, capture, reset_txready;
  char event[256];
  int value[256], ne;
} trace_t;
static void event(trace_t *t, char c, int v) {
  t->event[t->ne] = c;
  t->value[t->ne++] = v;
}
static void tx(void *p, int v) {
  trace_t *t = p;
  t->tx[t->n++] = v;
  if (t->capture) {
    t->reset_txready = i8251_txrdy(t->u);
    t->capture = 0;
  }
  event(t, 't', v);
}
static void rr(void *p, int v) {
  trace_t *t = p;
  t->rxready = v;
  event(t, 'x', v);
}
static void tr(void *p, int v) {
  trace_t *t = p;
  t->txready = v;
  event(t, 'y', v);
}
static void te(void *p, int v) {
  trace_t *t = p;
  t->empty = v;
  event(t, 'e', v);
}
static void sy(void *p, int v) {
  trace_t *t = p;
  t->syn = v;
  event(t, 's', v);
}
static void dtr(void *p, int v) { event(p, 'd', v); }
static void rts(void *p, int v) { event(p, 'r', v); }
static i8251_t *make(trace_t *t) {
  i8251_callbacks_t c;
  memset(&c, 0, sizeof(c));
  c.user = t;
  c.txd = tx;
  c.dtr = dtr;
  c.rts = rts;
  c.rxrdy = rr;
  c.txrdy = tr;
  c.txempty = te;
  c.syndet = sy;
  t->u = i8251_create(&c);
  return t->u;
}
static void clock_rx(i8251_t *u, int b) {
  i8251_write_rxd(u, b);
  i8251_write_rxc(u, 1);
  i8251_write_rxc(u, 0);
}
static void frame(i8251_t *u, unsigned data, unsigned bits, int parity,
                  int stop) {
  clock_rx(u, 0);
  for (unsigned i = 0; i < bits; i++)
    clock_rx(u, (data >> i) & 1);
  if (parity >= 0)
    clock_rx(u, parity);
  clock_rx(u, stop);
}
static unsigned ones(unsigned x) {
  unsigned n = 0;
  for (; x; x &= x - 1)
    n++;
  return n;
}
static void sync_char(i8251_t *u, unsigned data, unsigned bits, int even) {
  for (unsigned i = 0; i < bits; i++)
    clock_rx(u, (data >> i) & 1);
  clock_rx(u, (int)((ones(data) & 1) ^ (even ? 0 : 1)));
}
int main(void) {
  trace_t t = {0};
  i8251_t *u = make(&t);
  assert(u);
  i8251_write_cts(u, 0);
  i8251_write(u, 1, 0x4d);
  i8251_write(u, 1, 0x01);
  t.n = 0;
  i8251_write(u, 0, 0xa5);
  for (int i = 0; i < 10; i++) {
    i8251_write_txc(u, 1);
    i8251_write_txc(u, 0);
  }
  assert(t.n >= 10);
  const int expect[] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 1};
  for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++)
    assert(t.tx[i] == expect[i]);
  i8251_write(u, 1, 0x40);
  i8251_write(u, 1, 0x4d);
  i8251_write(u, 1, 0x04);
  frame(u, 0x53, 8, -1, 1);
  assert(i8251_read(u, 1) & 2);
  assert(i8251_read(u, 0) == 0x53);
  frame(u, 0x12, 8, -1, 1);
  frame(u, 0x34, 8, -1, 1);
  assert(i8251_read(u, 1) & 0x12);
  assert(i8251_read(u, 0) == 0x34);
  i8251_write(u, 1, 0x40);
  i8251_write(u, 1, 0x5d);
  i8251_write(u, 1, 0x14);
  frame(u, 0x11, 8, 1, 0);
  assert(i8251_read(u, 1) & 0x28); /* bad even parity and framing */
  (void)i8251_read(u, 0);
  /* Two-byte no-parity hunt consumes the source-ordered sync word. */
  i8251_write(u, 1, 0x40);
  i8251_write(u, 1, 0x0c);
  i8251_write(u, 1, 0x16);
  i8251_write(u, 1, 0x00);
  i8251_write(u, 1, 0x84);
  for (unsigned k = 0; k < 2; k++) {
    unsigned c = k ? 0x16 : 0x00;
    for (unsigned b = 0; b < 8; b++)
      clock_rx(u, (c >> b) & 1);
  }
  for (unsigned b = 0; b < 8; b++)
    clock_rx(u, (0x69 >> b) & 1);
  assert(i8251_read(u, 1) & 2);
  assert(i8251_read(u, 0) == 0x69);
  /* Matching sync characters with parity are suppressed from RX data. */
  i8251_write(u, 1, 0x40);
  i8251_write(u, 1, 0x9c);
  i8251_write(u, 1, 0x16);
  i8251_write(u, 1, 0x84);
  sync_char(u, 0x16, 8, 0);
  assert(!i8251_rxrdy(u));
  sync_char(u, 0x69, 8, 0);
  assert(i8251_rxrdy(u));
  assert(i8251_read(u, 0) == 0x69);
  i8251_write(u, 1, 0x40);
  i8251_write(u, 1, 0x3c);
  i8251_write(u, 1, 0x16);
  i8251_write(u, 1, 0x00);
  i8251_write(u, 1, 0x84);
  for (unsigned b = 0; b < 8; b++)
    clock_rx(u, (0x00 >> b) & 1);
  for (unsigned b = 0; b < 8; b++)
    clock_rx(u, (0x16 >> b) & 1);
  clock_rx(u, 1);
  assert(!i8251_rxrdy(u));
  sync_char(u, 0x53, 8, 1);
  assert(i8251_rxrdy(u));
  assert(i8251_read(u, 0) == 0x53);
  /* device_reset clears mode_byte but retains the generic async parity
     configuration. Synchronous sampling must consult mode_byte parity bits. */
  i8251_write(u, 1, 0x5d);
  i8251_write(u, 1, 0x04);
  i8251_reset(u);
  i8251_write(u, 1, 0x8c);
  i8251_write(u, 1, 0x16);
  i8251_write(u, 1, 0x84);
  for (unsigned b = 0; b < 8; b++)
    clock_rx(u, (0x16 >> b) & 1);
  for (unsigned b = 0; b < 8; b++)
    clock_rx(u, (0x53 >> b) & 1);
  assert(i8251_rxrdy(u));
  assert(i8251_read(u, 0) == 0x53);
  /* Reset callbacks see the old state until TXD/RTS/DTR have been emitted. */
  i8251_write(u, 1, 0x85);
  int before_reset = i8251_txrdy(u);
  assert(before_reset == 1);
  t.ne = 0;
  t.capture = 1;
  i8251_reset(u);
  assert(t.reset_txready == before_reset);
  assert(t.ne == 7);
  assert(memcmp(t.event, "trdtexy", 7) == 0);
  assert(t.value[0] == 1 && t.value[1] == 1 && t.value[2] == 1 &&
         t.value[3] == 1);
  assert(t.value[4] == 1 && t.value[5] == 0 && t.value[6] == 0);
  i8251_destroy(u);
  /* Generic receive-register reset keeps its prior RXD sample history. */
  trace_t h = {0};
  i8251_t *v = make(&h);
  assert(v);
  i8251_write(v, 1, 0x4d);
  i8251_write(v, 1, 0x04);
  clock_rx(v, 1);
  clock_rx(v, 0);
  i8251_reset(v);
  i8251_write(v, 1, 0x4d);
  i8251_write(v, 1, 0x04);
  frame(v, 0x53, 8, -1, 1);
  assert(!i8251_rxrdy(v));
  i8251_destroy(v);
  puts("i8251 pin waveform, reset and register tests passed");
  return 0;
}
