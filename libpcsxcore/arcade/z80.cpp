// license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller, Andrei I. Holub
// Standalone adapter for MAME's actual Z80 bus-cycle engine; see z80_LICENSE.txt.
#include "z80.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <new>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s8 = int8_t;
union PAIR16 {
    u16 w;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    struct {
        u8 h, l;
    } b;
#else
    struct {
        u8 l, h;
    } b;
#endif
};
#define CLEAR_LINE 0
#define LOGMASKED(...) ((void)0)
class z80_device;
struct Z80AddressCallback {
    z80_device *owner;
    unsigned kind;
    bool isunset() const;
    void operator()(u16 address, u8, u8) const;
};
struct Z80LineCallback {
    z80_device *owner;
    unsigned kind;
    void operator()(int value) const;
};
struct Z80IO {
    z80_device *owner;
    u8 read_interruptible(u16 port) const;
    void write_interruptible(u16 port, u8 value) const;
};
class z80_device {
  public:
#include "z80_declarations.inc"
    Z80Config cfg;
    Z80IO m_io;
    Z80AddressCallback m_nomreq_cb, m_refresh_cb;
    Z80LineCallback m_halt_cb, m_busack_cb, m_irqack_cb;
    uint64_t elapsed, instructions, interrupts, nmis, undocumented_count, unsupported_count;
    int64_t cycle_debt;
    int run_input_cycles;
    bool running, stop_after_instruction, fault;
    explicit z80_device(const Z80Config &c);
    void reset();
    void execute();
    uint64_t run(uint64_t cycles);
    uint64_t step();
    uint64_t total_cycles() const;
    u8 memory_read(u16 a) const { return cfg.memory_read ? cfg.memory_read(cfg.opaque, a) : 0xff; }
    void memory_write(u16 a, u8 v) {
        if (cfg.memory_write)
            cfg.memory_write(cfg.opaque, a, v);
    }
    u8 memory_opcode(u16 a) const {
        return cfg.opcode_read ? cfg.opcode_read(cfg.opaque, a) : memory_read(a);
    }
    u32 irq_vector() {
        interrupts++;
        return cfg.irq_ack ? cfg.irq_ack(cfg.opaque) : 0xff;
    }
    void daisy_call_reti_device() {
        if (cfg.reti)
            cfg.reti(cfg.opaque);
    }
    bool access_to_be_redone() const { return false; }
    void debugger_instruction_hook(u16) {}
    void debugger_wait_hook() {}
    bool instruction_end() {
        instructions++;
        m_ref = 0xffff00;
        return stop_after_instruction;
    }
    void logerror(const char *, ...) {
        unsupported_count++;
        if (cfg.diagnostic)
            cfg.diagnostic(cfg.opaque, Z80_DIAG_IM0_UNSUPPORTED, m_pc.w, m_tmp_irq_vector);
    }
};
struct Z80 : z80_device {
    explicit Z80(const Z80Config &c) : z80_device(c) {}
};

// clang-format off: macros must precede generated bodies.
#include "z80_macros.inc"
#include "z80_helpers.inc"
// clang-format on
bool Z80AddressCallback::isunset() const {
    return kind == 0 ? !owner->cfg.nomreq : !owner->cfg.refresh;
}
void Z80AddressCallback::operator()(u16 a, u8, u8) const {
    auto cb = kind == 0 ? owner->cfg.nomreq : owner->cfg.refresh;
    if (cb)
        cb(owner->cfg.opaque, a);
}
void Z80LineCallback::operator()(int v) const {
    if (owner->cfg.pin)
        owner->cfg.pin(owner->cfg.opaque, kind, v);
}
u8 Z80IO::read_interruptible(u16 p) const {
    return owner->cfg.io_read ? owner->cfg.io_read(owner->cfg.opaque, p) : 0xff;
}
void Z80IO::write_interruptible(u16 p, u8 v) const {
    if (owner->cfg.io_write)
        owner->cfg.io_write(owner->cfg.opaque, p, v);
}

z80_device::z80_device(const Z80Config &c) {
    std::memset(this, 0, sizeof(*this));
    cfg = c;
    m_io.owner = this;
    m_nomreq_cb = {this, 0};
    m_refresh_cb = {this, 1};
    m_halt_cb = {this, Z80_PIN_HALT};
    m_busack_cb = {this, Z80_PIN_BUSACK};
    m_irqack_cb = {this, Z80_PIN_IRQACK};
    m_m1_cycles = 4;
    m_mreq_cycles = 3;
    m_iorq_cycles = 4;
    set_f(0);
    m_ix.w = m_iy.w = 0xffff;
    m_f.z_val = 0;
    reset();
}
void z80_device::reset() {
    leave_halt();
    m_ref = 0xffff00;
    PC = 0;
    WZ = 0;
    I = R = R2 = 0;
    m_iff1 = m_iff2 = 0;
    set_service_attention<SA_NMI_PENDING, 0>();
    set_service_attention<SA_AFTER_EI, 0>();
    set_service_attention<SA_AFTER_LDAIR, 0>();
    cycle_debt = 0;
    fault = false;
}
void z80_device::execute() {
#include "z80_execution.inc"
}
uint64_t z80_device::total_cycles() const {
    return elapsed + (running ? uint64_t(run_input_cycles - m_icount) : 0);
}
uint64_t z80_device::run(uint64_t budget) {
    if (!budget)
        return 0;
    cycle_debt += int64_t(std::min<uint64_t>(budget, INT64_MAX / 2));
    uint64_t used = 0;
    stop_after_instruction = false;
    while (cycle_debt > 0) {
        int chunk = int(std::min<int64_t>(cycle_debt, INT_MAX / 2));
        run_input_cycles = m_icount = chunk;
        running = true;
        execute();
        running = false;
        uint64_t n = uint64_t(chunk - m_icount);
        elapsed += n;
        used += n;
        cycle_debt -= int64_t(n);
        if (!n)
            break;
    }
    return used;
}
uint64_t z80_device::step() {
    if (m_wait_state || m_busreq_state)
        return 0;
    stop_after_instruction = true;
    run_input_cycles = m_icount = 65536;
    running = true;
    execute();
    running = false;
    uint64_t n = uint64_t(run_input_cycles - m_icount);
    elapsed += n;
    cycle_debt = 0;
    stop_after_instruction = false;
    return n;
}
extern "C" {
Z80 *z80_create(const Z80Config *c) {
    if (!c)
        return nullptr;
    try {
        return new Z80(*c);
    } catch (...) {
        return nullptr;
    }
}
void z80_destroy(Z80 *c) { delete c; }
void z80_reset(Z80 *c) {
    if (c)
        c->reset();
}
uint64_t z80_run_cycles(Z80 *c, uint64_t n) { return c ? c->run(n) : 0; }
uint64_t z80_step_instruction(Z80 *c) { return c ? c->step() : 0; }
void z80_irq(Z80 *c, int s) {
    if (c) {
        c->m_irq_state = !!s;
        if (s)
            c->set_service_attention<z80_device::SA_IRQ_ON, 1>();
        else
            c->set_service_attention<z80_device::SA_IRQ_ON, 0>();
    }
}
void z80_nmi(Z80 *c, int s) {
    if (c) {
        if (!c->m_nmi_state && s && c->total_cycles())
            c->set_service_attention<z80_device::SA_NMI_PENDING, 1>();
        c->m_nmi_state = !!s;
    }
}
void z80_wait(Z80 *c, int s) {
    if (c)
        c->m_wait_state = !!s;
}
void z80_busreq(Z80 *c, int s) {
    if (c) {
        if (!c->m_busreq_state && s)
            c->set_service_attention<z80_device::SA_BUSREQ, 1>();
        c->m_busreq_state = !!s;
    }
}
void z80_get_status(Z80 *c, Z80Status *s) {
    if (!c || !s)
        return;
    std::memset(s, 0, sizeof(*s));
    s->pc = c->m_pc.w;
    s->previous_pc = c->m_prvpc.w;
    s->sp = c->m_sp.w;
    s->af = (c->m_af.b.h << 8) | c->get_f();
    s->bc = c->m_bc.w;
    s->de = c->m_de.w;
    s->hl = c->m_hl.w;
    s->ix = c->m_ix.w;
    s->iy = c->m_iy.w;
    s->af2 = c->m_af2.w;
    s->bc2 = c->m_bc2.w;
    s->de2 = c->m_de2.w;
    s->hl2 = c->m_hl2.w;
    s->wz = c->m_wz.w;
    s->i = c->m_i;
    s->r = (c->m_r & 0x7f) | (c->m_r2 & 0x80);
    s->im = c->m_im;
    s->iff1 = c->m_iff1;
    s->iff2 = c->m_iff2;
    s->halt = c->m_halt;
    s->irq = c->m_irq_state;
    s->nmi = c->m_nmi_state;
    s->wait = c->m_wait_state;
    s->busreq = c->m_busreq_state;
    s->busack = c->m_busack_state;
    s->execution_phase = c->m_ref;
    s->cycles = c->total_cycles();
    s->instructions = c->instructions;
    s->interrupts = c->interrupts;
    s->nmis = c->nmis;
    s->undocumented_opcodes = c->undocumented_count;
    s->unsupported_operations = c->unsupported_count;
    s->cycle_debt = c->cycle_debt;
}
uint16_t z80_get_register(Z80 *c, unsigned r) {
    if (!c)
        return 0;
    switch (r) {
    case Z80_PC:
        return c->m_pc.w;
    case Z80_SP:
        return c->m_sp.w;
    case Z80_AF:
        return (c->m_af.b.h << 8) | c->get_f();
    case Z80_A:
        return c->m_af.b.h;
    case Z80_F:
        return c->get_f();
    case Z80_B:
        return c->m_bc.b.h;
    case Z80_C:
        return c->m_bc.b.l;
    case Z80_D:
        return c->m_de.b.h;
    case Z80_E:
        return c->m_de.b.l;
    case Z80_H:
        return c->m_hl.b.h;
    case Z80_L:
        return c->m_hl.b.l;
    case Z80_BC:
        return c->m_bc.w;
    case Z80_DE:
        return c->m_de.w;
    case Z80_HL:
        return c->m_hl.w;
    case Z80_IX:
        return c->m_ix.w;
    case Z80_IY:
        return c->m_iy.w;
    case Z80_AF2:
        return c->m_af2.w;
    case Z80_BC2:
        return c->m_bc2.w;
    case Z80_DE2:
        return c->m_de2.w;
    case Z80_HL2:
        return c->m_hl2.w;
    case Z80_I:
        return c->m_i;
    case Z80_R:
        return (c->m_r & 127) | (c->m_r2 & 128);
    case Z80_IM:
        return c->m_im;
    case Z80_IFF1:
        return c->m_iff1;
    case Z80_IFF2:
        return c->m_iff2;
    case Z80_HALT:
        return c->m_halt;
    case Z80_WZ:
        return c->m_wz.w;
    default:
        return 0;
    }
}
void z80_set_register(Z80 *c, unsigned r, uint16_t v) {
    if (!c)
        return;
    switch (r) {
    case Z80_PC:
        c->m_pc.w = v;
        c->m_prvpc.w = v;
        c->m_ref = 0xffff00;
        c->cycle_debt = 0;
        break;
    case Z80_SP:
        c->m_sp.w = v;
        break;
    case Z80_AF:
        c->m_af.w = v;
        c->set_f(v);
        break;
    case Z80_A:
        c->m_af.b.h = v;
        break;
    case Z80_F:
        c->set_f(v);
        break;
    case Z80_B:
        c->m_bc.b.h = v;
        break;
    case Z80_C:
        c->m_bc.b.l = v;
        break;
    case Z80_D:
        c->m_de.b.h = v;
        break;
    case Z80_E:
        c->m_de.b.l = v;
        break;
    case Z80_H:
        c->m_hl.b.h = v;
        break;
    case Z80_L:
        c->m_hl.b.l = v;
        break;
    case Z80_BC:
        c->m_bc.w = v;
        break;
    case Z80_DE:
        c->m_de.w = v;
        break;
    case Z80_HL:
        c->m_hl.w = v;
        break;
    case Z80_IX:
        c->m_ix.w = v;
        break;
    case Z80_IY:
        c->m_iy.w = v;
        break;
    case Z80_AF2:
        c->m_af2.w = v;
        break;
    case Z80_BC2:
        c->m_bc2.w = v;
        break;
    case Z80_DE2:
        c->m_de2.w = v;
        break;
    case Z80_HL2:
        c->m_hl2.w = v;
        break;
    case Z80_I:
        c->m_i = v;
        break;
    case Z80_R:
        c->m_r = v & 127;
        c->m_r2 = v & 128;
        break;
    case Z80_IM:
        c->m_im = v % 3;
        break;
    case Z80_IFF1:
        c->m_iff1 = !!v;
        break;
    case Z80_IFF2:
        c->m_iff2 = !!v;
        break;
    case Z80_HALT:
        if (v)
            c->halt();
        else
            c->leave_halt();
        break;
    case Z80_WZ:
        c->m_wz.w = v;
        break;
    default:
        break;
    }
}
}
