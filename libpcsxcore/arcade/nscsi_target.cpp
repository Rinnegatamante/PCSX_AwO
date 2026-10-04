/* MAME-derived NSCSI target state machine. BSD-3-Clause; see nscsi_LICENSE.txt.
 */
#include "nscsi_target.h"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <vector>

struct NscsiTarget {
  enum State {
    IDLE,
    SETTLE,
    WAIT_SEL_LOW,
    NEXT,
    WAIT_MSG,
    WAIT_CMD,
    WAIT_IN,
    WAIT_OUT
  } state = IDLE;
  enum Sub { NONE, SEND_ACK1, SEND_ACK0, RECV_ACK1, RECV_ACK0 } sub = NONE;
  enum Action {
    ACT_MSG_OR_CMD,
    ACT_STATUS,
    ACT_MSG,
    ACT_DATA_IN,
    ACT_DATA_OUT,
    ACT_FREE
  };
  struct Control {
    Action a;
    unsigned p1 = 0;
    size_t p2 = 0;
  };
  NscsiBus *bus = nullptr;
  unsigned id = 0, initiator = 16;
  nscsi_target_backend_t backend{};
  uint32_t out_ctrl = 0, out_data = 0, ctrl = 0, data = 0;
  uint64_t now = 0, deadline = UINT64_MAX, last_timer_fire = 0;
  uint32_t faults = 0;
  std::array<Control, 32> q{};
  size_t qn = 0, qr = 0;
  std::array<uint8_t, 4096> cdb{};
  size_t cdb_len = 0, bufpos = 0, bufsize = 0;
  unsigned bufid = 0;
  uint8_t identify = 0;
  bool in_step = false, again = false, in_backend_command = false,
       in_backend_callback = false;
};
static void target_event(void *, uint32_t, uint32_t);
static bool push(NscsiTarget *t, NscsiTarget::Action a, unsigned p = 0,
                 size_t n = 0) {
  if (t->qn == t->q.size()) {
    t->faults |= NSCSI_TARGET_FAULT_QUEUE;
    return false;
  }
  t->q[t->qn++] = {a, p, n};
  return true;
}
static void drive_ctrl(NscsiTarget *t, uint32_t lines, uint32_t mask) {
  t->out_ctrl = (t->out_ctrl & ~mask) | (lines & mask);
  nscsi_bus_drive_ctrl(t->bus, t->id, lines, mask);
}
static void drive_data(NscsiTarget *t, uint32_t val) {
  t->out_data = val;
  nscsi_bus_drive_data(t->bus, t->id, val);
}
static void wait_ctrl(NscsiTarget *t, uint32_t lines, uint32_t mask) {
  nscsi_bus_wait_ctrl(t->bus, t->id, lines, mask);
}
static void run(NscsiTarget *t);
static void start_recv(NscsiTarget *t) {
  wait_ctrl(t, NSCSI_ACK, NSCSI_ACK);
  t->sub = NscsiTarget::RECV_ACK1;
  drive_ctrl(t, NSCSI_REQ, NSCSI_REQ);
}
static void start_send(NscsiTarget *t, uint8_t v) {
  wait_ctrl(t, NSCSI_ACK, NSCSI_ACK);
  t->sub = NscsiTarget::SEND_ACK1;
  drive_data(t, v);
  drive_ctrl(t, NSCSI_REQ, NSCSI_REQ);
}
static bool cdb_done(uint8_t op, size_t n) {
  if (!n)
    return false;
  switch (op >> 5) {
  case 0:
    return n == 6;
  case 1:
  case 2:
    return n == 10;
  case 3:
  case 4:
  case 6:
    return true;
  case 5:
    return n == 12;
  case 7:
    return n == 32;
  case 8:
  case 9:
    return n == 16;
  default:
    return true;
  }
}
static void command_done(NscsiTarget *t) {
  t->state = NscsiTarget::NEXT;
  t->sub = NscsiTarget::NONE;
  wait_ctrl(t, 0, NSCSI_ACK);
  if (t->backend.command) {
    t->in_backend_command = true;
    t->in_backend_callback = true;
    t->backend.command(t->backend.opaque, t, t->cdb.data(), t->cdb_len);
    t->in_backend_callback = false;
    t->in_backend_command = false;
  } else
    t->faults |= NSCSI_TARGET_FAULT_BACKEND;
  run(t);
}
static void run(NscsiTarget *t) {
  if (t->in_step) {
    t->again = true;
    return;
  }
  t->in_step = true;
  do {
    t->again = false;
    if (t->ctrl & NSCSI_RST) {
      drive_data(t, 0);
      drive_ctrl(t, 0, NSCSI_ALL);
      t->state = NscsiTarget::IDLE;
      t->sub = NscsiTarget::NONE;
      t->qn = t->qr = 0;
      t->identify = 0;
      t->cdb_len = t->bufpos = t->bufsize = 0;
      continue;
    }
    switch (t->sub) {
    case NscsiTarget::RECV_ACK1:
      if (t->ctrl & NSCSI_ACK) {
        uint8_t v = (uint8_t)t->data;
        if (t->state == NscsiTarget::WAIT_CMD) {
          if (t->cdb_len < t->cdb.size())
            t->cdb[t->cdb_len++] = v;
          else
            t->faults |= NSCSI_TARGET_FAULT_CDB_OVERFLOW;
        } else if (t->state == NscsiTarget::WAIT_MSG) {
          if (t->bufpos == 0) {
            if (v & 0x80)
              t->identify = v;
            else
              t->faults |= NSCSI_TARGET_FAULT_UNSUPPORTED_MESSAGE;
          }
        } else if (t->state == NscsiTarget::WAIT_OUT) {
          if (t->backend.write_data) {
            t->in_backend_callback = true;
            t->backend.write_data(t->backend.opaque, t->bufid, t->bufpos, v);
            t->in_backend_callback = false;
          } else
            t->faults |= NSCSI_TARGET_FAULT_BACKEND;
        }
        ++t->bufpos;
        t->sub = NscsiTarget::RECV_ACK0;
        drive_ctrl(t, 0, NSCSI_REQ);
      }
      break;
    case NscsiTarget::RECV_ACK0:
      if (!(t->ctrl & NSCSI_ACK)) {
        t->sub = NscsiTarget::NONE;
        wait_ctrl(t, 0, NSCSI_ACK);
        t->again = true;
      }
      break;
    case NscsiTarget::SEND_ACK1:
      if (t->ctrl & NSCSI_ACK) {
        t->sub = NscsiTarget::SEND_ACK0;
        drive_data(t, 0);
        drive_ctrl(t, 0, NSCSI_REQ);
      }
      break;
    case NscsiTarget::SEND_ACK0:
      if (!(t->ctrl & NSCSI_ACK)) {
        t->sub = NscsiTarget::NONE;
        wait_ctrl(t, 0, NSCSI_ACK);
        t->again = true;
      }
      break;
    default:
      break;
    }
    if (t->sub != NscsiTarget::NONE)
      continue;
    switch (t->state) {
    case NscsiTarget::IDLE:
      if ((t->ctrl & (NSCSI_SEL | NSCSI_BSY)) == NSCSI_SEL &&
          (t->data & (1u << t->id))) {
        t->initiator = 0;
        while (t->initiator < 16 &&
               (t->initiator == t->id || (t->data & (1u << t->initiator))))
          ++t->initiator;
        if (t->initiator == 16)
          t->initiator = UINT32_MAX;
        t->state = NscsiTarget::SETTLE;
        /* UINT64_MAX is the no-deadline sentinel, so equality is overflow
         * too.  Check before adding to avoid both wraparound and sentinel
         * aliasing at the exact horizon. */
        if (400 >= UINT64_MAX - t->now) {
          t->deadline = UINT64_MAX;
          t->faults |= NSCSI_TARGET_FAULT_TIME_OVERFLOW;
        } else {
          t->deadline = t->now + 400;
        }
      }
      break;
    case NscsiTarget::SETTLE:
      /* MAME calls step(false) for every watched control transition;
       * this state is not restricted to the timer callback. */
      if ((t->ctrl & (NSCSI_SEL | NSCSI_BSY)) == NSCSI_SEL) {
        t->state = NscsiTarget::WAIT_SEL_LOW;
        drive_ctrl(t, NSCSI_BSY, NSCSI_BSY);
      } else
        t->state = NscsiTarget::IDLE;
      break;
    case NscsiTarget::WAIT_SEL_LOW:
      if (!(t->ctrl & NSCSI_SEL)) {
        push(t, NscsiTarget::ACT_MSG_OR_CMD);
        t->state = NscsiTarget::NEXT;
        t->again = true;
      }
      break;
    case NscsiTarget::NEXT:
      if (t->qr == t->qn)
        break;
      {
        auto c = t->q[t->qr++];
        if (t->qr == t->qn)
          t->qr = t->qn = 0;
        switch (c.a) {
        case NscsiTarget::ACT_MSG_OR_CMD:
          t->bufpos = 0;
          t->cdb_len = 0;
          t->state = (t->ctrl & NSCSI_ATN) ? NscsiTarget::WAIT_MSG
                                           : NscsiTarget::WAIT_CMD;
          drive_ctrl(
              t, (t->ctrl & NSCSI_ATN) ? (NSCSI_MSG | NSCSI_CTL) : NSCSI_CTL,
              NSCSI_PHASE_MASK);
          start_recv(t);
          break;
        case NscsiTarget::ACT_STATUS:
          drive_ctrl(t, NSCSI_INP | NSCSI_CTL, NSCSI_PHASE_MASK);
          t->state = NscsiTarget::NEXT;
          start_send(t, (uint8_t)c.p1);
          break;
        case NscsiTarget::ACT_MSG:
          drive_ctrl(t, NSCSI_INP | NSCSI_CTL | NSCSI_MSG, NSCSI_PHASE_MASK);
          t->state = NscsiTarget::NEXT;
          start_send(t, (uint8_t)c.p1);
          break;
        case NscsiTarget::ACT_DATA_IN:
          t->bufid = c.p1;
          t->bufsize = c.p2;
          t->bufpos = 0;
          drive_ctrl(t, NSCSI_INP, NSCSI_PHASE_MASK);
          t->state = t->bufsize ? NscsiTarget::WAIT_IN : NscsiTarget::NEXT;
          t->again = true;
          break;
        case NscsiTarget::ACT_DATA_OUT:
          t->bufid = c.p1;
          t->bufsize = c.p2;
          t->bufpos = 0;
          drive_ctrl(t, 0, NSCSI_PHASE_MASK);
          t->state = t->bufsize ? NscsiTarget::WAIT_OUT : NscsiTarget::NEXT;
          t->again = true;
          break;
        case NscsiTarget::ACT_FREE:
          drive_data(t, 0);
          wait_ctrl(t, NSCSI_BSY | NSCSI_SEL | NSCSI_RST, NSCSI_ALL);
          drive_ctrl(t, 0, NSCSI_ALL);
          t->state = NscsiTarget::IDLE;
          break;
        }
      }
      break;
    case NscsiTarget::WAIT_CMD:
      if (t->ctrl & NSCSI_SEL)
        break;
      if (t->ctrl & NSCSI_ATN) {
        t->state = NscsiTarget::IDLE;
        break;
      }
      if (t->cdb_len) {
        int done = -1;
        if (t->backend.command_done) {
          t->in_backend_callback = true;
          done = t->backend.command_done(t->backend.opaque, t->cdb[0], t->cdb_len);
          t->in_backend_callback = false;
          if (done < -1 || done > 1) {
            t->faults |= NSCSI_TARGET_FAULT_BACKEND;
            break;
          }
        }
        if (done < 0) done = cdb_done(t->cdb[0], t->cdb_len);
        if (done) {
          command_done(t);
          break;
        }
      }
      start_recv(t);
      break;
    case NscsiTarget::WAIT_MSG:
      if (t->ctrl & NSCSI_SEL)
        break;
      if (!(t->ctrl & NSCSI_ATN)) {
        t->state = NscsiTarget::WAIT_CMD;
        t->cdb_len = 0;
        t->bufpos = 0;
        drive_ctrl(t, NSCSI_CTL, NSCSI_PHASE_MASK);
        start_recv(t);
      } else
        start_recv(t);
      break;
    case NscsiTarget::WAIT_IN:
      if (t->bufpos >= t->bufsize) {
        t->state = NscsiTarget::NEXT;
        t->again = true;
        break;
      }
      {
        uint8_t v = 0;
        if (t->backend.read_data) {
          t->in_backend_callback = true;
          v = t->backend.read_data(t->backend.opaque, t->bufid, t->bufpos);
          t->in_backend_callback = false;
        }
        if (!t->backend.read_data)
          t->faults |= NSCSI_TARGET_FAULT_BACKEND;
        ++t->bufpos;
        if (t->bufpos == t->bufsize)
          t->state = NscsiTarget::NEXT;
        start_send(t, v);
      }
      break;
    case NscsiTarget::WAIT_OUT:
      if (t->bufpos >= t->bufsize) {
        t->state = NscsiTarget::NEXT;
        t->again = true;
        break;
      }
      start_recv(t);
      break;
    default:
      break;
    }
  } while (t->again);
  t->in_step = false;
}
static void target_event(void *opaque, uint32_t ctrl, uint32_t data) {
  auto *t = (NscsiTarget *)opaque;
  t->ctrl = ctrl | t->out_ctrl;
  t->data = data | t->out_data;
  run(t);
}
extern "C" {
NscsiTarget *nscsi_target_create(NscsiBus *b, unsigned id,
                                 const nscsi_target_backend_t *be) {
  if (!b || id >= 16 || !be || !be->command)
    return nullptr;
  auto *t = new (std::nothrow) NscsiTarget;
  if (!t)
    return nullptr;
  t->bus = b;
  t->id = id;
  t->backend = *be;
  nscsi_bus_ops_t ops{t, target_event};
  if (!nscsi_bus_attach(b, id, &ops)) {
    delete t;
    return nullptr;
  }
  nscsi_bus_wait_ctrl(b, id, NSCSI_SEL | NSCSI_BSY | NSCSI_RST, NSCSI_ALL);
  return t;
}
void nscsi_target_destroy(NscsiTarget *t) {
  if (!t)
    return;
  if (t->in_backend_callback) {
    t->faults |= NSCSI_TARGET_FAULT_REENTRANT;
    return;
  }
  nscsi_bus_detach(t->bus, t->id);
  delete t;
}
void nscsi_target_reset(NscsiTarget *t) {
  if (!t)
    return;
  if (t->in_backend_callback) {
    t->faults |= NSCSI_TARGET_FAULT_REENTRANT;
    return;
  }
  t->state = NscsiTarget::IDLE;
  t->sub = NscsiTarget::NONE;
  t->qn = t->qr = 0;
  t->identify = 0;
  t->bufpos = t->bufsize = t->cdb_len = 0;
  drive_data(t, 0);
  drive_ctrl(t, 0, NSCSI_ALL);
  wait_ctrl(t, NSCSI_SEL | NSCSI_BSY | NSCSI_RST, NSCSI_ALL);
}
void nscsi_target_advance_to(NscsiTarget *t, uint64_t now) {
  if (!t)
    return;
  if (t->in_backend_callback) {
    t->faults |= NSCSI_TARGET_FAULT_REENTRANT;
    return;
  }
  if (now < t->now) {
    t->faults |= NSCSI_TARGET_FAULT_TIME_REWIND;
    return;
  }
  const uint64_t budget = now;
  while (t->deadline != UINT64_MAX && t->deadline <= budget) {
    t->now = t->deadline;
    t->last_timer_fire = t->deadline;
    t->deadline = UINT64_MAX;
    t->ctrl = nscsi_bus_ctrl_external(t->bus, t->id) | t->out_ctrl;
    t->data = nscsi_bus_data_external(t->bus, t->id) | t->out_data;
    run(t);
  }
  t->now = budget;
}
static int queue_phase(NscsiTarget *t, NscsiTarget::Action a, unsigned p,
                       size_t n) {
  if (!t)
    return 0;
  if (!t->in_backend_command) {
    t->faults |= NSCSI_TARGET_FAULT_PHASE_BACKEND;
    return 0;
  }
  if (!push(t, a, p, n))
    return 0;
  if (t->state == NscsiTarget::NEXT)
    run(t);
  return !t->faults;
}
int nscsi_target_data_in(NscsiTarget *t, unsigned b, size_t n) {
  if (t && !t->backend.read_data) {
    t->faults |= NSCSI_TARGET_FAULT_BACKEND;
    return 0;
  }
  return queue_phase(t, NscsiTarget::ACT_DATA_IN, b, n);
}
int nscsi_target_data_out(NscsiTarget *t, unsigned b, size_t n) {
  if (t && !t->backend.write_data) {
    t->faults |= NSCSI_TARGET_FAULT_BACKEND;
    return 0;
  }
  return queue_phase(t, NscsiTarget::ACT_DATA_OUT, b, n);
}
int nscsi_target_status_complete(NscsiTarget *t, uint8_t s) {
  if (!t)
    return 0;
  if (!t->in_backend_command) {
    t->faults |= NSCSI_TARGET_FAULT_PHASE_BACKEND;
    return 0;
  }
  if (t->qn + 3 > t->q.size()) {
    t->faults |= NSCSI_TARGET_FAULT_QUEUE;
    return 0;
  }
  push(t, NscsiTarget::ACT_STATUS, s);
  push(t, NscsiTarget::ACT_MSG, NSCSI_MESSAGE_COMMAND_COMPLETE);
  push(t, NscsiTarget::ACT_FREE);
  if (t->state == NscsiTarget::NEXT)
    run(t);
  return !t->faults;
}
int nscsi_target_message_in(NscsiTarget *t, uint8_t m) {
  return queue_phase(t, NscsiTarget::ACT_MSG, m, 0);
}
int nscsi_target_bus_free(NscsiTarget *t) {
  return queue_phase(t, NscsiTarget::ACT_FREE, 0, 0);
}
void nscsi_target_get_status(const NscsiTarget *t, nscsi_target_status_t *s) {
  if (!s)
    return;
  *s = {};
  if (t) {
    s->now_ns = t->now;
    s->timer_deadline_ns = t->deadline;
    s->last_timer_fire_ns = t->last_timer_fire;
    s->faults = t->faults;
    s->scsi_id = t->id;
    s->initiator_id = t->initiator;
    s->state = t->state;
    s->cdb_length = (uint16_t)t->cdb_len;
    s->identify = t->identify;
  }
}
}
