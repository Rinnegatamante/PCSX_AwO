#include "../../libpcsxcore/arcade/mb89371.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct trace {
  mb89371_t *d;
  int tx[2][512], ntx[2], rr[2], tr[2], empty[2], syn[2];
  uint64_t time[2][512];
  unsigned event_channel[1024], event_count;
  int event_level[1024];
  uint64_t event_time[1024];
} trace_t;
static void record_tx(trace_t *t, unsigned ch, int v, int n) {
  t->tx[ch][n] = v;
  t->time[ch][n] = mb89371_time_ns(t->d);
  unsigned event = t->event_count++;
  t->event_channel[event] = ch;
  t->event_level[event] = v;
  t->event_time[event] = t->time[ch][n];
}
static void tx0(void *p, int v) {
  trace_t *t = p;
  int n = t->ntx[0]++;
  record_tx(t, 0, v, n);
}
static void tx1(void *p, int v) {
  trace_t *t = p;
  int n = t->ntx[1]++;
  record_tx(t, 1, v, n);
}
static void rr0(void *p, int v) { ((trace_t *)p)->rr[0] = v; }
static void rr1(void *p, int v) { ((trace_t *)p)->rr[1] = v; }
static void tr0(void *p, int v) { ((trace_t *)p)->tr[0] = v; }
static void tr1(void *p, int v) { ((trace_t *)p)->tr[1] = v; }
static void te0(void *p, int v) { ((trace_t *)p)->empty[0] = v; }
static void te1(void *p, int v) { ((trace_t *)p)->empty[1] = v; }
static void sy0(void *p, int v) { ((trace_t *)p)->syn[0] = v; }
static void sy1(void *p, int v) { ((trace_t *)p)->syn[1] = v; }
static void rxclock(mb89371_t *d, unsigned c, int b) {
  mb89371_write_rxd(d, c, b);
  mb89371_write_rxc(d, c, 1);
  mb89371_write_rxc(d, c, 0);
}
static void rxclock_only(mb89371_t *d, unsigned c) {
  mb89371_write_rxc(d, c, 1);
  mb89371_write_rxc(d, c, 0);
}
static mb89371_t *create(trace_t *t, uint32_t hz) {
  mb89371_callbacks_t c;
  memset(&c, 0, sizeof(c));
  c.user = t;
  c.txd[0] = tx0;
  c.txd[1] = tx1;
  mb89371_t *d = mb89371_create(hz, &c);
  t->d = d;
  return d;
}
static void test_timing(void) {
  assert(mb89371_create(0, NULL) == NULL);
  trace_t whole = {0}, split = {0};
  mb89371_t *a = create(&whole, 1000000), *b = create(&split, 1000000);
  assert(a && b);
  mb89371_t *dev[2] = {a, b};
  trace_t *traces[2] = {&whole, &split};
  for (int k = 0; k < 2; k++)
    for (unsigned ch = 0; ch < 2; ch++) {
      mb89371_write(dev[k], ch, 1, 0x4d);
      mb89371_write(dev[k], ch, 1, 0x01);
      mb89371_write_cts(dev[k], ch, 0);
      mb89371_write(dev[k], ch, 2, ch ? 1 : 0);
      mb89371_write(dev[k], ch, 3, 0x02);
      mb89371_write(dev[k], ch, 0, ch ? 0x3c : 0xa5);
      traces[k]->ntx[ch] = 0;
    }
  mb89371_advance(a, 20000);
  const uint64_t slices[] = {1, 777, 2000, 2001, 4999, 5000, 15001, 20000};
  for (unsigned i = 0; i < sizeof(slices) / sizeof(slices[0]); i++)
    mb89371_advance(b, slices[i]);
  for (unsigned ch = 0; ch < 2; ch++) {
    assert(whole.ntx[ch] == split.ntx[ch]);
    for (int i = 0; i < whole.ntx[ch]; i++) {
      assert(whole.tx[ch][i] == split.tx[ch][i]);
      assert(whole.time[ch][i] == split.time[ch][i]);
    }
  }
  assert(whole.event_count == split.event_count);
  for (unsigned event = 0; event < whole.event_count; event++) {
    assert(whole.event_channel[event] == split.event_channel[event]);
    assert(whole.event_level[event] == split.event_level[event]);
    assert(whole.event_time[event] == split.event_time[event]);
    if (event)
      assert(whole.event_time[event] >= whole.event_time[event - 1]);
  }
  /* Per-channel timestamps merge into one monotonic event stream; callback
     queries observe the event time, rather than the caller's final budget. */
  assert(whole.ntx[0] > 1 && whole.time[0][0] == 2000 &&
         whole.time[1][0] == 4000);
  assert(mb89371_time_ns(a) == 20000 && mb89371_time_ns(b) == 20000);
  /* Undefined register offsets stay open bus and do not alias offset zero. */
  uint8_t tx_before = mb89371_read(a, 0, 0);
  assert(mb89371_read(a, 0, 4) == 0xff);
  mb89371_write(a, 0, 4, 0x5a);
  assert(mb89371_read(a, 0, 0) == tx_before);
  mb89371_advance(a, 19999);
  assert(mb89371_faults(a) & MB89371_FAULT_TIME_REWIND);
  assert(mb89371_time_ns(a) == 20000);
  mb89371_write(a, 0, 2, 30);
  assert(mb89371_faults(a) & MB89371_FAULT_INVALID_BAUD);
  mb89371_destroy(a);
  mb89371_destroy(b);
  trace_t fasttrace = {0};
  mb89371_t *fast = create(&fasttrace, 1200000000);
  assert(fast);
  mb89371_write(fast, 0, 3, 0x02);
  assert(mb89371_faults(fast) & MB89371_FAULT_SUBNANOSECOND_CLOCK);
  mb89371_advance(fast, 100);
  assert(mb89371_time_ns(fast) == 100);
  mb89371_destroy(fast);
  trace_t slowtrace = {0};
  mb89371_t *slow = create(&slowtrace, 1000);
  assert(slow);
  mb89371_write(slow, 0, 2, 10);
  mb89371_write(slow, 0, 3, 0x02);
  assert(mb89371_faults(slow) & MB89371_FAULT_ZERO_RATE);
  mb89371_destroy(slow);
  trace_t overflowtrace = {0};
  mb89371_t *overflow = create(&overflowtrace, 1000000);
  assert(overflow);
  mb89371_advance(overflow, UINT64_MAX - 500);
  mb89371_write(overflow, 0, 3, 0x02);
  assert(mb89371_faults(overflow) & MB89371_FAULT_TIME_OVERFLOW);
  mb89371_destroy(overflow);

  /* A due edge at UINT64_MAX remains processable, and a 2.7 ns half-period
   * follows ceil(k * period): the first two rising edges are at 3 and 9 ns. */
  trace_t rationaltrace = {0};
  mb89371_t *rational = create(&rationaltrace, 370370370);
  assert(rational);
  mb89371_write(rational, 0, 1, 0x4d);
  mb89371_write(rational, 0, 1, 0x01);
  mb89371_write_cts(rational, 0, 0);
  mb89371_write(rational, 0, 3, 0x02);
  mb89371_write(rational, 0, 0, 0xaa);
  rationaltrace.ntx[0] = 0;
  mb89371_advance(rational, 20);
  assert(rationaltrace.ntx[0] >= 3);
  assert(rationaltrace.time[0][0] == 6);
  assert(rationaltrace.time[0][1] == 11);
  mb89371_destroy(rational);

  trace_t lastticktrace = {0};
  mb89371_t *lasttick = create(&lastticktrace, 1000000);
  assert(lasttick);
  uint64_t next = 0;
  assert(!mb89371_next_event_ns(lasttick, &next) && next == UINT64_MAX);
  mb89371_write(lasttick, 0, 1, 0x4d);
  mb89371_write(lasttick, 0, 1, 0x01);
  mb89371_write_cts(lasttick, 0, 0);
  mb89371_advance(lasttick, UINT64_MAX - 1000);
  mb89371_write(lasttick, 0, 3, 0x02);
  assert(mb89371_next_event_ns(lasttick, &next) && next == UINT64_MAX);
  mb89371_write(lasttick, 0, 0, 0x00);
  lastticktrace.ntx[0] = 0;
  mb89371_advance(lasttick, UINT64_MAX);
  assert(!mb89371_next_event_ns(lasttick, &next));
  assert(mb89371_time_ns(lasttick) == UINT64_MAX);
  assert(mb89371_faults(lasttick) & MB89371_FAULT_TIME_OVERFLOW);
  /* The valid final rising edge must reach the child before scheduling the
   * next edge overflows. Switching to external clocks and driving low then
   * emits the buffered start bit; a skipped rising edge would emit nothing. */
  mb89371_write(lasttick, 0, 3, 0x00);
  mb89371_write_txc(lasttick, 0, 0);
  assert(lastticktrace.ntx[0] == 1 && lastticktrace.tx[0][0] == 0);
  mb89371_destroy(lasttick);
}
int main(void) {
  trace_t t = {0};
  mb89371_callbacks_t c;
  memset(&c, 0, sizeof(c));
  c.user = &t;
  c.txd[0] = tx0;
  c.txd[1] = tx1;
  c.rxready[0] = rr0;
  c.rxready[1] = rr1;
  c.txready[0] = tr0;
  c.txready[1] = tr1;
  c.txempty[0] = te0;
  c.txempty[1] = te1;
  c.syndet[0] = sy0;
  c.syndet[1] = sy1;
  mb89371_t *d = mb89371_create(1000000, &c);
  t.d = d;
  assert(d);
  /* Independent externally clocked transmitters. */
  mb89371_write(d, 0, 1, 0x4d);
  mb89371_write(d, 0, 1, 0x01);
  mb89371_write(d, 1, 1, 0x4d);
  mb89371_write(d, 1, 1, 0x01);
  mb89371_write_cts(d, 0, 0);
  mb89371_write_cts(d, 1, 0);
  mb89371_write(d, 0, 3, 0x00);
  t.tr[0] = -1;
  mb89371_write(d, 0, 1, 0x01);
  assert(t.tr[0] == -1);
  mb89371_write(d, 0, 3, 0x20);
  mb89371_write(d, 0, 1, 0x01);
  assert(t.tr[0] == 1);
  t.ntx[0] = t.ntx[1] = 0;
  mb89371_write(d, 0, 0, 0xa5);
  mb89371_write(d, 1, 0, 0x3c);
  for (int n = 0; n < 10; n++) {
    mb89371_write_txc(d, 0, 1);
    mb89371_write_txc(d, 0, 0);
    mb89371_write_txc(d, 1, 1);
    mb89371_write_txc(d, 1, 0);
  }
  assert(t.ntx[0] >= 10 && t.ntx[1] >= 10);
  assert(t.tx[0][0] == 0 && t.tx[1][0] == 0);
  assert(t.tx[0][1] == 1 && t.tx[1][1] == 0);
  /* Mask source quirk: TXEMPTY uses bit5. RXREADY uses bit6. */
  mb89371_write(d, 0, 3, 0x10);
  mb89371_write_txc(d, 0, 1);
  mb89371_write_txc(d, 0, 0);
  assert(t.empty[0] == 0);
  mb89371_write(d, 0, 3, 0x20);
  mb89371_write_txc(d, 0, 1);
  mb89371_write_txc(d, 0, 0);
  assert(t.empty[0] == 1);
  mb89371_write(d, 0, 3, 0x40);
  mb89371_write(d, 0, 1, 0x40);
  mb89371_write(d, 0, 1, 0x4d);
  mb89371_write(d, 0, 1, 0x04);
  rxclock(d, 0, 0);
  for (int b = 0; b < 8; b++)
    rxclock(d, 0, (0x55 >> b) & 1);
  rxclock(d, 0, 1);
  assert(mb89371_rxrdy(d, 0));
  assert(t.rr[0]);
  /* Internal BRG runs channel 0 from the floored 500 kHz clock; channel 1
   * remains external. */
  mb89371_write(d, 0, 3, 0x02);
  mb89371_write(d, 0, 2, 0);
  mb89371_write(d, 0, 1, 0x05);
  mb89371_write(d, 0, 0, 0x81);
  mb89371_advance(d, 20000);
  assert(t.ntx[0] > 9);
  int before = t.ntx[1];
  mb89371_advance(d, 40000);
  assert(t.ntx[1] == before);
  /* Loopback routes TXD to RXD internally while retaining clock selection. */
  mb89371_write(d, 0, 3, 0x08);
  mb89371_write(d, 0, 1, 0x05);
  mb89371_write(d, 0, 0, 0x69);
  (void)mb89371_read(d, 0, 0);
  for (int n = 0; n < 11; n++) {
    mb89371_write_txc(d, 0, 1);
    rxclock_only(d, 0);
    mb89371_write_txc(d, 0, 0);
  }
  assert(mb89371_read(d, 0, 3) == 0x08);
  assert(mb89371_rxrdy(d, 0));
  assert(mb89371_read(d, 0, 0) == 0x69);
  /* Wrapper reset resets child USARTs and preserves wrapper mode/baud
   * registers. */
  mb89371_write(d, 1, 2, 7);
  mb89371_write(d, 1, 3, 0x12);
  mb89371_reset(d);
  assert(mb89371_read(d, 1, 2) == 7 && mb89371_read(d, 1, 3) == 0x12);
  mb89371_destroy(d);
  test_timing();
  puts("mb89371 dual-channel, reset and clock tests passed");
  return 0;
}
