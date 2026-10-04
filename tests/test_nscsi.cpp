#include "ncr53cf96.h"
#include "nscsi_bus.h"
#include "nscsi_target.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Wire {
  NscsiBus *bus{};
  unsigned id{};
  uint32_t ctrl = 0, data = 0;
  unsigned events = 0;
};
static void changed(void *p, uint32_t c, uint32_t d) {
  auto *w = (Wire *)p;
  w->ctrl = c;
  w->data = d;
  ++w->events;
}
static void destroy_from_callback(void *p, uint32_t, uint32_t) {
  nscsi_bus_destroy(static_cast<NscsiBus *>(p));
}
struct Medium {
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> cdb;
  unsigned calls = 0;
};
static void command(void *p, NscsiTarget *t, const uint8_t *c, size_t n) {
  auto *m = (Medium *)p;
  m->cdb.assign(c, c + n);
  ++m->calls;
  if (n == 6 && c[0] == 0x12) {
    nscsi_target_data_in(t, 0, (size_t)c[4]);
    nscsi_target_status_complete(t, NSCSI_STATUS_GOOD);
  } else if (n == 6 && c[0] == 0x0a) {
    nscsi_target_data_out(t, 1, 3);
    nscsi_target_status_complete(t, NSCSI_STATUS_GOOD);
  } else
    nscsi_target_status_complete(t, NSCSI_STATUS_CHECK_CONDITION);
}
static uint8_t read_data(void *p, unsigned, size_t i) {
  auto *m = (Medium *)p;
  return i < m->bytes.size() ? m->bytes[i] : 0;
}
static void write_data(void *p, unsigned, size_t i, uint8_t v) {
  auto *m = (Medium *)p;
  if (m->bytes.size() <= i)
    m->bytes.resize(i + 1);
  m->bytes[i] = v;
}
static void reentrant_backend_command(void *, NscsiTarget *t, const uint8_t *,
                                      size_t) {
  nscsi_target_reset(t);
  nscsi_target_destroy(t);
  nscsi_target_advance_to(t, 1);
  (void)nscsi_target_status_complete(t, NSCSI_STATUS_GOOD);
}
static void mutate_ctrl(NscsiBus *b, unsigned id, uint32_t v,
                        uint32_t mask = NSCSI_ALL) {
  nscsi_bus_drive_ctrl(b, id, v, mask);
  assert(nscsi_bus_flush(b));
}
static void mutate_data(NscsiBus *b, unsigned id, uint32_t v) {
  nscsi_bus_drive_data(b, id, v);
  assert(nscsi_bus_flush(b));
}

static void resolver_preserves_every_edge_and_filters_wait_mask() {
  NscsiBus *b = nscsi_bus_create();
  assert(b);
  Wire w{};
  nscsi_bus_ops_t o{&w, changed};
  assert(nscsi_bus_attach(b, 2, &o));
  assert(nscsi_bus_attach(b, 7, &o));
  nscsi_bus_wait_ctrl(b, 2, NSCSI_REQ, NSCSI_REQ);
  nscsi_bus_drive_ctrl(b, 7, NSCSI_REQ, NSCSI_REQ);
  nscsi_bus_drive_ctrl(b, 7, 0, NSCSI_REQ);
  assert(nscsi_bus_flush(b));
  assert(w.events == 2);
  assert(w.ctrl == 0);
  nscsi_bus_wait_ctrl(b, 2, 0, NSCSI_REQ);
  nscsi_bus_drive_ctrl(b, 7, NSCSI_REQ, NSCSI_REQ);
  assert(nscsi_bus_flush(b));
  assert(w.events == 2);
  nscsi_bus_destroy(b);
}
static void resolver_detach_generation_and_queue_overflow_are_bounded() {
  NscsiBus *b = nscsi_bus_create();
  assert(b);
  Wire old{}, replacement{}, source{};
  nscsi_bus_ops_t old_ops{&old, changed}, new_ops{&replacement, changed},
      source_ops{&source, changed};
  assert(nscsi_bus_attach(b, 2, &old_ops));
  assert(nscsi_bus_attach(b, 7, &source_ops));
  nscsi_bus_wait_ctrl(b, 2, NSCSI_REQ, NSCSI_REQ);
  nscsi_bus_drive_ctrl(b, 7, NSCSI_REQ, NSCSI_REQ); // queued for old endpoint
  nscsi_bus_detach(b, 2);
  assert(nscsi_bus_attach(b, 2, &new_ops));
  assert(nscsi_bus_flush(b));
  assert(old.events == 0 &&
         replacement.events == 0); // stale event not rerouted

  nscsi_bus_wait_ctrl(b, 2, NSCSI_REQ, NSCSI_REQ);
  for (unsigned i = 0; i < 8194; ++i)
    nscsi_bus_drive_ctrl(b, 7, (i & 1) ? 0 : NSCSI_REQ, NSCSI_REQ);
  nscsi_bus_status_t status{};
  nscsi_bus_get_status(b, &status);
  assert(!nscsi_bus_flush(b));
  assert(status.faults & NSCSI_BUS_FAULT_QUEUE_OVERFLOW);
  nscsi_bus_destroy(b);

  b = nscsi_bus_create();
  assert(b);
  Wire observer{}, driver{};
  nscsi_bus_ops_t observer_ops{&observer, changed},
      driver_ops{&driver, changed};
  assert(nscsi_bus_attach(b, 1, &observer_ops));
  assert(nscsi_bus_attach(b, 2, &driver_ops));
  nscsi_bus_wait_ctrl(b, 1, NSCSI_BSY, NSCSI_BSY);
  nscsi_bus_drive_ctrl(b, 2, NSCSI_BSY, NSCSI_BSY);
  assert(nscsi_bus_flush(b) && observer.ctrl == NSCSI_BSY);
  nscsi_bus_detach(b, 2); // releasing a driven line notifies remaining peers
  assert(nscsi_bus_flush(b) && observer.events == 2 && observer.ctrl == 0);
  nscsi_bus_destroy(b);

  b = nscsi_bus_create();
  assert(b);
  nscsi_bus_ops_t destroy_ops{b, destroy_from_callback},
      source_ops2{&source, changed};
  assert(nscsi_bus_attach(b, 2, &destroy_ops));
  assert(nscsi_bus_attach(b, 7, &source_ops2));
  nscsi_bus_wait_ctrl(b, 2, NSCSI_REQ, NSCSI_REQ);
  nscsi_bus_drive_ctrl(b, 7, NSCSI_REQ, NSCSI_REQ);
  assert(!nscsi_bus_flush(b));
  nscsi_bus_get_status(b, &status);
  assert(status.faults & NSCSI_BUS_FAULT_REENTRANT_DESTROY);
  nscsi_bus_destroy(b); // destruction is safe once dispatch has returned
}

static void target_selection_identify_full_cdb_inquiry_and_completion() {
  NscsiBus *b = nscsi_bus_create();
  assert(b);
  Wire host{};
  host.bus = b;
  host.id = 7;
  nscsi_bus_ops_t ho{&host, changed};
  assert(nscsi_bus_attach(b, 7, &ho));
  Medium m;
  m.bytes = {'T', 'E', 'S', 'T'};
  nscsi_target_backend_t be{&m, command, read_data, write_data, nullptr};
  auto *t = nscsi_target_create(b, 4, &be);
  assert(t);
  /* Select target 4 with ATN and transmit an IDENTIFY message (0x80). */
  mutate_data(b, 7, (1u << 7) | (1u << 4));
  mutate_ctrl(b, 7, NSCSI_SEL | NSCSI_ATN);
  nscsi_target_advance_to(t, 400);
  assert(nscsi_bus_ctrl(b) & NSCSI_BSY);
  mutate_ctrl(b, 7, NSCSI_ATN); /* SEL drops; target begins MSG OUT. */
  auto send = [&](uint8_t v, uint32_t atn = 0) {
    mutate_data(b, 7, v);
    mutate_ctrl(b, 7, atn | NSCSI_ACK);
    mutate_ctrl(b, 7, atn);
  };
  /* Drop ATN with ACK so the target observes it on the same ACK-falling
   * transition, as the source step() state machine does. */
  mutate_data(b, 7, 0x80);
  mutate_ctrl(b, 7, NSCSI_ATN | NSCSI_ACK);
  mutate_ctrl(b, 7, NSCSI_ATN);
  mutate_data(b, 7, 0x08);
  mutate_ctrl(b, 7, NSCSI_ATN | NSCSI_ACK);
  mutate_ctrl(b, 7, 0);
  const uint8_t cdb[6] = {0x12, 0, 0, 0, 4, 0};
  for (uint8_t x : cdb)
    send(x);
  assert(m.calls == 1 && m.cdb == std::vector<uint8_t>(cdb, cdb + 6));
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == NSCSI_PHASE_DATA_IN);
  std::vector<uint8_t> got;
  auto recv = [&]() {
    uint8_t v = (uint8_t)nscsi_bus_data_external(b, 7);
    mutate_ctrl(b, 7, NSCSI_ACK);
    mutate_ctrl(b, 7, 0);
    return v;
  };
  for (unsigned i = 0; i < 4; i++)
    got.push_back(recv());
  assert(got == m.bytes);
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == (NSCSI_INP | NSCSI_CTL));
  assert(recv() == 0);
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_IN);
  assert(recv() == NSCSI_MESSAGE_COMMAND_COMPLETE);
  assert((nscsi_bus_ctrl(b) & NSCSI_BSY) == 0);
  nscsi_target_status_t s{};
  nscsi_target_get_status(t, &s);
  assert(s.identify == 0x80 && s.cdb_length == 6 && s.initiator_id == 0 &&
         !s.faults);
  /* A second transaction exercises the target-to-host DATA OUT path and
   * backend write callback, then the same status/message/bus-free sequence. */
  mutate_data(b, 7, (1u << 7) | (1u << 4));
  mutate_ctrl(b, 7, NSCSI_SEL);
  nscsi_target_advance_to(t, 800);
  mutate_ctrl(b, 7, 0);
  const uint8_t write_cdb[6] = {0x0a, 0, 0, 0, 3, 0};
  for (uint8_t x : write_cdb)
    send(x);
  const uint8_t payload[3] = {0x19, 0x28, 0x37};
  for (uint8_t x : payload)
    send(x);
  assert(m.calls == 2 &&
         m.cdb == std::vector<uint8_t>(write_cdb, write_cdb + 6));
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS);
  assert(recv() == 0);
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_IN);
  assert(recv() == 0);
  assert((nscsi_bus_ctrl(b) & NSCSI_BSY) == 0);
  assert(m.bytes[0] == payload[0] && m.bytes[1] == payload[1] &&
         m.bytes[2] == payload[2]);
  nscsi_target_destroy(t);
  nscsi_bus_destroy(b);
}

struct NcrWire {
  NscsiBus *b{};
  Ncr53cf96 *c{};
  unsigned id = 7;
  uint32_t own_ctrl = 0, own_data = 0, wait = 0;
};
static void ncr_dc(void *p, uint32_t v, uint32_t mask) {
  auto *w = (NcrWire *)p;
  w->own_ctrl = (w->own_ctrl & ~mask) | (v & mask);
  nscsi_bus_drive_ctrl(w->b, w->id, v, mask);
}
static void ncr_dd(void *p, uint32_t v) {
  auto *w = (NcrWire *)p;
  w->own_data = v;
  nscsi_bus_drive_data(w->b, w->id, v);
}
static void ncr_wait(void *p, uint32_t v, uint32_t mask) {
  auto *w = (NcrWire *)p;
  w->wait = (w->wait & ~mask) | (v & mask);
  nscsi_bus_wait_ctrl(w->b, w->id, v, mask);
}
static void ncr_changed(void *p, uint32_t c, uint32_t d) {
  auto *w = (NcrWire *)p;
  ncr53cf96_bus_changed(w->c, c, d);
}
static void no_irq(void *, int) {}
static void controller_observes_real_target_selection() {
  NscsiBus *b = nscsi_bus_create();
  assert(b);
  NcrWire w{};
  w.b = b;
  nscsi_bus_ops_t ops{&w, ncr_changed};
  assert(nscsi_bus_attach(b, 7, &ops));
  ncr53cf96_bus_ops_t no{};
  no.opaque = &w;
  no.drive_ctrl = ncr_dc;
  no.drive_data = ncr_dd;
  no.wait_ctrl = ncr_wait;
  no.irq = no_irq;
  no.drq = no_irq;
  w.c = ncr53cf96_create(16000000, &no);
  assert(w.c);
  Medium m;
  m.bytes = {'O', 'K'};
  nscsi_target_backend_t be{&m, command, read_data, write_data, nullptr};
  auto *t = nscsi_target_create(b, 4, &be);
  assert(t);
  const uint8_t cdb[6] = {0x12, 0, 0, 0, 2, 0};
  for (uint8_t v : cdb)
    ncr53cf96_write(w.c, 2, v);
  ncr53cf96_write(w.c, 8, 7);
  ncr53cf96_write(w.c, 4, 4);
  ncr53cf96_write(w.c, 3, 0x41);
  /* Run the controller at 16 MHz and map absolute chip cycles to a shared
   * 62.5 ns grid for the target's integer-nanosecond timer. */
  for (uint64_t cycle = 1; cycle <= 2200 && m.calls == 0; cycle++) {
    nscsi_target_advance_to(t, (cycle * 125) / 2);
    nscsi_bus_flush(b);
    ncr53cf96_advance_to(w.c, cycle);
    nscsi_bus_flush(b);
  }
  ncr53cf96_status_t ns{};
  ncr53cf96_get_status(w.c, &ns);
  assert(m.calls == 1 && m.cdb == std::vector<uint8_t>(cdb, cdb + 6));
  auto run_ncr_ticks = [&]() {
    ncr53cf96_status_t s{};
    ncr53cf96_get_status(w.c, &s);
    const uint64_t first = s.cycles + 1;
    for (uint64_t cycle = first; cycle < first + 128; ++cycle) {
      nscsi_target_advance_to(t, (cycle * 125) / 2);
      nscsi_bus_flush(b);
      ncr53cf96_advance_to(w.c, cycle);
      nscsi_bus_flush(b);
    }
  };
  run_ncr_ticks(); // Finish CD_SELECT on the DATA IN phase change.
  ncr53cf96_get_status(w.c, &ns);
  assert(ns.interrupt_status & 0x08); // the select sequence completed
  (void)ncr53cf96_read(w.c, 5);       // acknowledge and chain the next command
  /* DMA a two-byte target DATA IN phase through the real NCR, then fetch
   * STATUS and COMMAND COMPLETE through its real phase sequencer. */
  ncr53cf96_write(w.c, 11, 0x08);
  ncr53cf96_write(w.c, 0, 2);
  ncr53cf96_write(w.c, 1, 0);
  ncr53cf96_write(w.c, 14, 0);
  ncr53cf96_write(w.c, 3, 0x90);
  nscsi_bus_flush(b);
  run_ncr_ticks();
  ncr53cf96_get_status(w.c, &ns);
  assert(ns.fifo_count == 2);
  assert(ncr53cf96_dma_read(w.c) == 'O');
  assert(ncr53cf96_dma_read(w.c) == 'K');
  (void)ncr53cf96_read(w.c, 5); // acknowledge data transfer completion

  ncr53cf96_write(w.c, 0, 1);
  ncr53cf96_write(w.c, 1, 0);
  ncr53cf96_write(w.c, 14, 0);
  ncr53cf96_write(w.c, 3, 0x90);
  nscsi_bus_flush(b);
  run_ncr_ticks();
  assert(ncr53cf96_dma_read(w.c) == NSCSI_STATUS_GOOD);
  (void)ncr53cf96_read(w.c, 5); // acknowledge status transfer completion
  ncr53cf96_write(w.c, 3, 0x11);
  nscsi_bus_flush(b);
  run_ncr_ticks();
  assert(ncr53cf96_read(w.c, 2) == NSCSI_MESSAGE_COMMAND_COMPLETE);
  /* CI_COMPLETE reads COMMAND COMPLETE but leaves ACK asserted.  The source
   * NCR only releases the bus after the initiator explicitly accepts it. */
  ncr53cf96_write(w.c, 3, 0x12); // CI_MSG_ACCEPT
  nscsi_bus_flush(b);
  run_ncr_ticks();
  ncr53cf96_get_status(w.c, &ns);
  assert((ns.interrupt_status & 0x20) != 0); // I_DISCONNECT
  assert(nscsi_bus_ctrl(b) == 0); // target and initiator released bus
  ncr53cf96_destroy(w.c);
  nscsi_target_destroy(t);
  nscsi_bus_destroy(b);
}
static void target_timer_horizon_and_rewind_are_diagnosed() {
  NscsiBus *b = nscsi_bus_create();
  assert(b);
  Wire host{};
  nscsi_bus_ops_t host_ops{&host, changed};
  assert(nscsi_bus_attach(b, 7, &host_ops));
  Medium m;
  nscsi_target_backend_t be{&m, command, read_data, write_data, nullptr};
  auto *t = nscsi_target_create(b, 4, &be);
  assert(t);

  /* At equality, adding the settle interval would alias UINT64_MAX's idle
   * timer sentinel, so it must fail closed rather than silently never fire. */
  nscsi_target_advance_to(t, UINT64_MAX - 400);
  mutate_data(b, 7, (1u << 7) | (1u << 4));
  mutate_ctrl(b, 7, NSCSI_SEL);
  nscsi_target_status_t status{};
  nscsi_target_get_status(t, &status);
  assert(status.faults & NSCSI_TARGET_FAULT_TIME_OVERFLOW);
  assert(status.timer_deadline_ns == UINT64_MAX);
  nscsi_target_advance_to(t, UINT64_MAX - 401);
  nscsi_target_get_status(t, &status);
  assert(status.faults & NSCSI_TARGET_FAULT_TIME_REWIND);
  nscsi_target_destroy(t);
  nscsi_bus_destroy(b);
}

static void reset_during_selection_cancels_settle_and_absent_id_stays_absent() {
  NscsiBus *b = nscsi_bus_create();
  Wire h{};
  nscsi_bus_ops_t ho{&h, changed};
  assert(nscsi_bus_attach(b, 7, &ho));
  Medium m;
  nscsi_target_backend_t be{&m, command, read_data, write_data, nullptr};
  auto *t = nscsi_target_create(b, 4, &be);
  assert(t);
  mutate_data(b, 7, (1u << 7) | (1u << 4));
  mutate_ctrl(b, 7, NSCSI_SEL);
  nscsi_target_advance_to(t, 100);
  mutate_ctrl(b, 7, NSCSI_RST);
  nscsi_target_status_t s{};
  nscsi_target_get_status(t, &s);
  assert(s.timer_deadline_ns == 400 && !(nscsi_bus_ctrl(b) & NSCSI_BSY));
  mutate_ctrl(b, 7, 0);
  nscsi_target_advance_to(t, 1000);
  nscsi_target_get_status(t, &s);
  assert(s.last_timer_fire_ns == 400 && s.now_ns == 1000);
  assert(!(nscsi_bus_ctrl(b) & NSCSI_BSY));
  /* No endpoint at ID 3: resolver does not synthesize BSY/REQ or READY. */
  mutate_data(b, 7, (1u << 7) | (1u << 3));
  mutate_ctrl(b, 7, NSCSI_SEL);
  nscsi_target_advance_to(t, 1400);
  assert(!(nscsi_bus_ctrl(b) & NSCSI_BSY));
  nscsi_target_destroy(t);
  nscsi_bus_destroy(b);
}
static void watched_control_change_can_end_settle_before_deadline() {
  NscsiBus *b = nscsi_bus_create();
  Wire h{};
  nscsi_bus_ops_t ho{&h, changed};
  assert(nscsi_bus_attach(b, 7, &ho));
  Medium m;
  nscsi_target_backend_t be{&m, command, read_data, write_data, nullptr};
  auto *t = nscsi_target_create(b, 4, &be);
  assert(t);
  mutate_data(b, 7, (1u << 7) | (1u << 4));
  mutate_ctrl(b, 7, NSCSI_SEL);
  /* MAME's own wait mask normally watches SEL/BSY/RST. Add ATN here to
   * exercise step(false) from an arbitrary delivered control edge. */
  nscsi_bus_wait_ctrl(b, 4, NSCSI_SEL | NSCSI_BSY | NSCSI_RST | NSCSI_ATN,
                      NSCSI_ALL);
  nscsi_target_status_t s{};
  nscsi_target_get_status(t, &s);
  assert(s.timer_deadline_ns == 400 && !(nscsi_bus_ctrl(b) & NSCSI_BSY));
  mutate_ctrl(b, 7, NSCSI_SEL | NSCSI_ATN);
  assert(nscsi_bus_ctrl(b) & NSCSI_BSY);
  nscsi_target_get_status(t, &s);
  assert(s.timer_deadline_ns == 400 && s.state == 2);
  nscsi_target_advance_to(t, 400);
  assert(nscsi_bus_ctrl(b) & NSCSI_BSY);
  mutate_ctrl(b, 7, NSCSI_ATN);
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_OUT);
  nscsi_target_reset(t);
  nscsi_bus_flush(b);
  nscsi_target_destroy(t);
  nscsi_bus_destroy(b);
}
static void backend_reentry_is_diagnosed_without_destroying_target() {
  NscsiBus *b = nscsi_bus_create();
  Wire host{};
  nscsi_bus_ops_t host_ops{&host, changed};
  assert(nscsi_bus_attach(b, 7, &host_ops));
  nscsi_target_backend_t be{};
  be.command = reentrant_backend_command;
  auto *t = nscsi_target_create(b, 4, &be);
  assert(t);
  mutate_data(b, 7, (1u << 7) | (1u << 4));
  mutate_ctrl(b, 7, NSCSI_SEL);
  nscsi_target_advance_to(t, 400);
  mutate_ctrl(b, 7, 0);
  const uint8_t cdb[6] = {0x12, 0, 0, 0, 0, 0};
  for (uint8_t value : cdb) {
    mutate_data(b, 7, value);
    mutate_ctrl(b, 7, NSCSI_ACK);
    mutate_ctrl(b, 7, 0);
  }
  nscsi_target_status_t status{};
  nscsi_target_get_status(t, &status);
  assert(status.faults & NSCSI_TARGET_FAULT_REENTRANT);
  assert((nscsi_bus_ctrl(b) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS);
  nscsi_target_destroy(t); // the rejected callback destroy left it alive
  nscsi_bus_destroy(b);
}
static void real_ncr_absent_target_times_out_on_resolved_bus() {
  NscsiBus *b = nscsi_bus_create();
  assert(b);
  NcrWire w{};
  w.b = b;
  nscsi_bus_ops_t ops{&w, ncr_changed};
  assert(nscsi_bus_attach(b, 7, &ops));
  ncr53cf96_bus_ops_t no{};
  no.opaque = &w;
  no.drive_ctrl = ncr_dc;
  no.drive_data = ncr_dd;
  no.wait_ctrl = ncr_wait;
  no.irq = no_irq;
  no.drq = no_irq;
  w.c = ncr53cf96_create(16000000, &no);
  assert(w.c);
  ncr53cf96_write(w.c, 8, 7);
  ncr53cf96_write(w.c, 4, 4);
  ncr53cf96_write(w.c, 3, 0x41);
  ncr53cf96_advance_to(w.c, 2044);
  nscsi_bus_flush(b);
  ncr53cf96_status_t s{};
  ncr53cf96_get_status(w.c, &s);
  assert((s.interrupt_status & 0x20) && !(s.interrupt_status & 0x08));
  assert(s.bus_ctrl == 0);
  ncr53cf96_destroy(w.c);
  nscsi_bus_destroy(b);
}
int main() {
  resolver_preserves_every_edge_and_filters_wait_mask();
  resolver_detach_generation_and_queue_overflow_are_bounded();
  target_selection_identify_full_cdb_inquiry_and_completion();
  controller_observes_real_target_selection();
  reset_during_selection_cancels_settle_and_absent_id_stays_absent();
  watched_control_change_can_end_settle_before_deadline();
  backend_reentry_is_diagnosed_without_destroying_target();
  real_ncr_absent_target_times_out_on_resolved_bus();
  target_timer_horizon_and_rewind_are_diagnosed();
  std::puts("NSCSI bus/target protocol tests passed");
}
