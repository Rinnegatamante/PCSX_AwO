/* Standalone MAME-derived NSCSI wired-OR bus with ordered deferred callbacks.
 * Copyright Olivier Galibert. BSD-3-Clause; see nscsi_LICENSE.txt. */
#include "nscsi_bus.h"
#include <array>
#include <memory>
#include <new>
#include <vector>

struct NscsiBus {
  struct Endpoint {
    bool attached = false;
    uint32_t ctrl = 0, wait = 0, data = 0;
    uint64_t generation = 0;
    nscsi_bus_ops_t ops{};
  };
  struct Event {
    unsigned id;
    uint64_t generation;
    uint32_t ctrl, data;
  };
  std::array<Endpoint, 16> ep{};
  std::vector<Event> events;
  size_t head = 0;
  uint64_t mutations = 0, notifications = 0;
  uint64_t next_generation = 1;
  uint32_t faults = 0;
  uint32_t aggregate_ctrl = 0, aggregate_data = 0;
  bool dispatching = false;
};

static uint32_t ctrl_or(const NscsiBus *b) {
  uint32_t v = 0;
  for (auto &e : b->ep)
    if (e.attached)
      v |= e.ctrl;
  return v;
}
static uint32_t data_or(const NscsiBus *b) {
  uint32_t v = 0;
  for (auto &e : b->ep)
    if (e.attached)
      v |= e.data;
  return v;
}
static uint32_t ctrl_external(const NscsiBus *b, unsigned id) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 16; i++)
    if (i != id && b->ep[i].attached)
      v |= b->ep[i].ctrl;
  return v;
}
static uint32_t data_external(const NscsiBus *b, unsigned id) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 16; i++)
    if (i != id && b->ep[i].attached)
      v |= b->ep[i].data;
  return v;
}
static void queue_control_change(NscsiBus *b, unsigned source_id,
                                 uint32_t changed) {
  if (!changed)
    return;
  for (unsigned i = 0; i < 16; i++) {
    const auto &e = b->ep[i];
    if (i == source_id || !e.attached || !(e.wait & changed))
      continue;
    if (b->events.size() >= 8192) {
      b->faults |= NSCSI_BUS_FAULT_QUEUE_OVERFLOW;
      return;
    }
    b->events.push_back(
        {i, e.generation, ctrl_external(b, i), data_external(b, i)});
  }
}
extern "C" {
NscsiBus *nscsi_bus_create(void) {
  try {
    auto b = std::make_unique<NscsiBus>();
    b->events.reserve(8192);
    return b.release();
  } catch (...) {
    return nullptr;
  }
}
void nscsi_bus_destroy(NscsiBus *b) {
  if (!b)
    return;
  if (b->dispatching) {
    b->faults |= NSCSI_BUS_FAULT_REENTRANT_DESTROY;
    return;
  }
  delete b;
}
int nscsi_bus_attach(NscsiBus *b, unsigned id, const nscsi_bus_ops_t *ops) {
  if (!b || id >= 16 || !ops || b->ep[id].attached) {
    if (b)
      b->faults |= NSCSI_BUS_FAULT_BAD_ENDPOINT;
    return 0;
  }
  if (!b->next_generation) {
    b->faults |= NSCSI_BUS_FAULT_BAD_ENDPOINT;
    return 0;
  }
  b->ep[id].attached = true;
  b->ep[id].generation = b->next_generation++;
  b->ep[id].ops = *ops;
  return 1;
}
void nscsi_bus_detach(NscsiBus *b, unsigned id) {
  if (!b || id >= 16 || !b->ep[id].attached)
    return;
  const uint32_t old_ctrl = b->aggregate_ctrl;
  b->ep[id] = NscsiBus::Endpoint{};
  ++b->mutations;
  const uint32_t new_ctrl = ctrl_or(b);
  const uint32_t changed = old_ctrl ^ new_ctrl;
  b->aggregate_ctrl = new_ctrl;
  b->aggregate_data = data_or(b);
  queue_control_change(b, id, changed);
}
void nscsi_bus_drive_ctrl(NscsiBus *b, unsigned id, uint32_t lines,
                          uint32_t mask) {
  if (!b || id >= 16 || !b->ep[id].attached) {
    if (b)
      b->faults |= NSCSI_BUS_FAULT_BAD_ENDPOINT;
    return;
  }
  auto &e = b->ep[id];
  e.ctrl = (e.ctrl & ~mask) | (lines & mask);
  ++b->mutations;
  const uint32_t now = ctrl_or(b), changed = b->aggregate_ctrl ^ now;
  b->aggregate_ctrl = now;
  b->aggregate_data = data_or(b);
  queue_control_change(b, id, changed);
}
void nscsi_bus_drive_data(NscsiBus *b, unsigned id, uint32_t data) {
  if (!b || id >= 16 || !b->ep[id].attached) {
    if (b)
      b->faults |= NSCSI_BUS_FAULT_BAD_ENDPOINT;
    return;
  }
  b->ep[id].data = data;
  ++b->mutations;
  b->aggregate_data = data_or(b);
}
void nscsi_bus_wait_ctrl(NscsiBus *b, unsigned id, uint32_t lines,
                         uint32_t mask) {
  if (!b || id >= 16 || !b->ep[id].attached) {
    if (b)
      b->faults |= NSCSI_BUS_FAULT_BAD_ENDPOINT;
    return;
  }
  auto &e = b->ep[id];
  e.wait = (e.wait & ~mask) | (lines & mask);
}
uint32_t nscsi_bus_ctrl(const NscsiBus *b) { return b ? b->aggregate_ctrl : 0; }
uint32_t nscsi_bus_data(const NscsiBus *b) { return b ? b->aggregate_data : 0; }
uint32_t nscsi_bus_ctrl_external(const NscsiBus *b, unsigned id) {
  return b && id < 16 ? ctrl_external(b, id) : 0;
}
uint32_t nscsi_bus_data_external(const NscsiBus *b, unsigned id) {
  return b && id < 16 ? data_external(b, id) : 0;
}
int nscsi_bus_flush(NscsiBus *b) {
  if (!b || b->dispatching)
    return b ? 1 : 0;
  b->dispatching = true;
  while (b->head < b->events.size()) {
    const auto ev = b->events[b->head++];
    auto &e = b->ep[ev.id];
    if (e.attached && e.generation == ev.generation && e.ops.changed) {
      ++b->notifications;
      e.ops.changed(e.ops.opaque, ev.ctrl, ev.data);
    }
    if (b->faults & NSCSI_BUS_FAULT_QUEUE_OVERFLOW)
      break;
  }
  b->events.clear();
  b->head = 0;
  b->dispatching = false;
  return b->faults == 0;
}
void nscsi_bus_get_status(const NscsiBus *b, nscsi_bus_status_t *s) {
  if (!s)
    return;
  *s = {};
  if (b) {
    s->mutations = b->mutations;
    s->notifications = b->notifications;
    s->faults = b->faults;
  }
}
}
