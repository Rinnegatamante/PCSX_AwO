// SPDX-License-Identifier: BSD-3-Clause
// MAME interpreter copyright Vas Crabb; standalone adapter pcsx_aow contributors.
#include "dsp16.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
template <class T> constexpr unsigned BIT(T v, unsigned n) { return (uint64_t(v) >> n) & 1; }
template <unsigned N, class T, class... B> constexpr u16 bitswap(T v, B... b) {
    unsigned pos[] = {unsigned(b)...};
    u16 r = 0;
    for (unsigned i = 0; i < N; ++i)
        r = (r << 1) | BIT(v, pos[i]);
    return r;
}
struct emu_fatalerror : std::runtime_error {
    template <class... A> emu_fatalerror(const char *f, A... a) : std::runtime_error(format(f, a...)) {}
    template <class... A> static std::string format(const char *f, A... a) {
        char s[1024];
        if constexpr (sizeof...(A))
            std::snprintf(s, sizeof(s), f, a...);
        else
            std::snprintf(s, sizeof(s), "%s", f);
        return s;
    }
};
#define DSP16_REQUIRE(x)                                                                                     \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw emu_fatalerror("DSP16: invalid instruction/state: %s", #x);                                \
    } while (0)
#define LOGPIO(...) ((void)0)
#define LOGSIO(...) ((void)0)
#define LOGINT(...) ((void)0)
enum {
    CLEAR_LINE = 0,
    ASSERT_LINE = 1,
    DSP16_INT_LINE = 0,
    DSP16_ICK_LINE,
    DSP16_ILD_LINE,
    DSP16_OCK_LINE,
    DSP16_OLD_LINE
};
struct DSP16 {
    class core_state;
    DSP16Config cfg;
    std::unique_ptr<core_state> m_core;
    std::vector<u16> rom;
    std::array<u16, 2048> m_workram{};
    u16 m_workram_mask;
    u64 instructions = 0, warnings = 0, faults = 0;
    bool halted = false;
    bool unsupported_rx_warned = false;
    struct line_cb {
        DSP16 *d;
        unsigned pin;
        void operator()(int x) const {
            if (d->cfg.pin_write)
                d->cfg.pin_write(d->cfg.opaque, pin, x);
        }
    };
    struct pio_read_cb {
        DSP16 *d;
        bool isunset() const { return !d->cfg.pio_read; }
        u16 operator()(unsigned s, u16) const {
            return d->cfg.pio_read ? d->cfg.pio_read(d->cfg.opaque, s) : 0xffff;
        }
    };
    struct pio_write_cb {
        DSP16 *d;
        void operator()(unsigned s, u16 v, u16) const {
            if (d->cfg.pio_write)
                d->cfg.pio_write(d->cfg.opaque, s, v);
        }
    };
    struct bus_cb {
        DSP16 *d;
        void operator()(unsigned s, u16 v, u16 mask) const {
            if (d->cfg.parallel_bus_write)
                d->cfg.parallel_bus_write(d->cfg.opaque, s, v, mask);
        }
    };
    line_cb m_iack_cb{this, DSP16_PIN_IACK}, m_ick_cb{this, DSP16_PIN_ICK}, m_ild_cb{this, DSP16_PIN_ILD},
        m_do_cb{this, DSP16_PIN_DO}, m_ock_cb{this, DSP16_PIN_OCK}, m_old_cb{this, DSP16_PIN_OLD},
        m_ose_cb{this, DSP16_PIN_OSE}, m_psel_cb{this, DSP16_PIN_PSEL}, m_pids_cb{this, DSP16_PIN_PIDS},
        m_pods_cb{this, DSP16_PIN_PODS};
    pio_read_cb m_pio_r_cb{this};
    pio_write_cb m_pio_w_cb{this};
    bus_cb m_pdb_w_cb{this};
    bool debugger_enabled() const { return false; }
    void debugger_instruction_hook(u16) {}
    void standard_irq_callback(int, u16) {}
    void logerror(const char *f, ...) {
        char msg[1024];
        va_list a;
        va_start(a, f);
        vsnprintf(msg, sizeof(msg), f, a);
        va_end(a);
        ++warnings;
        if (cfg.diagnostic)
            cfg.diagnostic(cfg.opaque, DSP16_DIAG_WARNING, m_st_pcbase, msg);
    }
    void fault(const char *msg) {
        halted = true;
        ++faults;
        if (cfg.diagnostic)
            cfg.diagnostic(cfg.opaque, DSP16_DIAG_FAULT, m_st_pcbase, msg);
    }
    u16 program_read(u16 a) {
        if (m_exm_in && (cfg.model == DSP16_MODEL_16 || a < 0x1000))
            return rom[a & (rom.size() - 1)];
        if (!cfg.external_read)
            throw emu_fatalerror("DSP16: external ROM read %04X without callback", a);
        return cfg.external_read(cfg.opaque, a);
    }
    DSP16(const DSP16Config &);
    void device_reset();
    void execute_set_input(int, int);
    u16 pio_r();
    void pio_w(u16);
#include "dsp16_declarations.inc"
};
using dsp16_device_base = DSP16;
#define ENUM_OPS(T)                                                                                          \
    constexpr T operator~(T v) { return T(~unsigned(v)); }                                                   \
    constexpr T operator&(T a, T b) { return T(unsigned(a) & unsigned(b)); }                                 \
    constexpr T operator|(T a, T b) { return T(unsigned(a) | unsigned(b)); }                                 \
    T &operator&=(T &a, T b) { return a = a & b; }                                                           \
    T &operator|=(T &a, T b) { return a = a | b; }
ENUM_OPS(DSP16::flags)
ENUM_OPS(DSP16::sio_flags)
#include "dsp16_core.inc"
#include "dsp16_engine.inc"
DSP16::DSP16(const DSP16Config &c)
    : cfg(c), m_core(new core_state(c.model == DSP16_MODEL_16 ? 9 : 16)), rom(c.rom, c.rom + c.rom_words),
      m_workram_mask(c.model == DSP16_MODEL_16 ? 0x1ff : 0x7ff) {
    m_cache_mode = cache::NONE;
    m_phase = phase::PURGE;
    std::fill(std::begin(m_int_enable), std::end(m_int_enable), 0);
    m_flags = FLAGS_NONE;
    m_cache_ptr = m_cache_limit = m_cache_iterations = 0;
    std::fill(std::begin(m_cache), std::end(m_cache), 0);
    m_rom_data = 0;
    m_exm_in = 1;
    m_int_in = 0;
    m_iack_out = 1;
    m_ick_in = m_ock_in = 1;
    m_ild_in = m_old_in = 0;
    m_do_out = m_ose_out = m_pids_out = m_pods_out = 1;
    m_psel_out = 0;
    m_sio_sioc = m_sio_obuf = m_sio_osr = m_sio_ofsr = 0;
    m_sio_clk = 1;
    m_sio_clk_div = m_sio_clk_res = m_sio_ld_div = 0;
    m_sio_ld = 1;
    m_sio_flags = SIO_FLAGS_NONE;
    m_pio_pioc = 8;
    m_pio_pdx_in = m_pio_pdx_out = 0;
    m_pio_pids_cnt = m_pio_pods_cnt = 0;
    m_cache_pcbase = m_st_pcbase = m_st_genflags = 0;
    m_st_yh = m_st_ah[0] = m_st_ah[1] = 0;
    m_st_yl = m_st_al[0] = m_st_al[1] = 0;
    device_reset();
}
extern "C" DSP16 *dsp16_create(const DSP16Config *c) {
    if (!c || c->model < 0 || c->model > 1 || !c->rom || c->rom_words != size_t(c->model ? 0x1000 : 0x800))
        return nullptr;
    try {
        return new DSP16(*c);
    } catch (...) {
        return nullptr;
    }
}
extern "C" void dsp16_destroy(DSP16 *d) { delete d; }
extern "C" void dsp16_reset(DSP16 *d) {
    if (d) {
        d->halted = false;
        d->unsupported_rx_warned = false;
        d->device_reset();
    }
}
extern "C" uint64_t dsp16_run_cycles(DSP16 *d, uint64_t n) {
    if (!d || d->halted)
        return 0;
    uint64_t before = d->m_core->cycles;
    try {
        while (n) {
            int slice = int(std::min<uint64_t>(n, 0x3fffffff));
            d->m_core->icount = slice;
            while (d->m_core->icount_remaining()) {
                switch (d->m_cache_mode) {
                case DSP16::cache::NONE:
                    d->execute_some_rom<false, false>();
                    break;
                case DSP16::cache::LOAD:
                    d->execute_some_rom<false, true>();
                    break;
                case DSP16::cache::EXECUTE:
                    d->execute_some_cache<false>();
                    break;
                }
            }
            n -= slice;
        }
    } catch (const std::exception &e) {
        d->fault(e.what());
    }
    return d->m_core->cycles - before;
}
extern "C" int dsp16_set_input(DSP16 *d, unsigned i, int level) {
    if (!d || i > DSP16_INPUT_EXM)
        return -1;
    try {
        if (i == DSP16_INPUT_EXM)
            d->m_exm_in = !!level;
        else
            d->execute_set_input(i, !!level);
        return 0;
    } catch (const std::exception &e) {
        d->fault(e.what());
        return -1;
    }
}
extern "C" uint16_t dsp16_host_pio_read(DSP16 *d) { return d ? d->pio_r() : 0xffff; }
extern "C" void dsp16_host_pio_write(DSP16 *d, uint16_t v) {
    if (d)
        d->pio_w(v);
}
extern "C" int dsp16_read_register(DSP16 *d, unsigned r, uint16_t *v) {
    if (!d || !v || r > 0x3f)
        return -1;
    try {
        *v = d->get_r(r << 4);
        return 0;
    } catch (const std::exception &e) {
        d->fault(e.what());
        return -1;
    }
}
extern "C" int dsp16_write_register(DSP16 *d, unsigned r, uint16_t v) {
    if (!d || r > 0x3f)
        return -1;
    try {
        d->set_r(r << 4, v);
        return 0;
    } catch (const std::exception &e) {
        d->fault(e.what());
        return -1;
    }
}
extern "C" uint16_t dsp16_ram_read(const DSP16 *d, uint16_t a) {
    return d ? d->m_workram[a & d->m_workram_mask] : 0xffff;
}
extern "C" void dsp16_ram_write(DSP16 *d, uint16_t a, uint16_t v) {
    if (d)
        d->m_workram[a & d->m_workram_mask] = v;
}
extern "C" int dsp16_get_registers(const DSP16 *d, DSP16Registers *r) {
    if (!d || !r)
        return -1;
    auto &c = *d->m_core;
    r->pc = c.xaau_pc;
    r->pt = c.xaau_pt;
    r->pr = c.xaau_pr;
    r->pi = c.xaau_pi;
    r->i = c.xaau_i;
    std::copy(std::begin(c.yaau_r), std::end(c.yaau_r), r->r);
    r->rb = c.yaau_rb;
    r->re = c.yaau_re;
    r->j = c.yaau_j;
    r->k = c.yaau_k;
    r->x = c.dau_x;
    r->y = c.dau_y;
    r->p = c.dau_p;
    std::copy(std::begin(c.dau_a), std::end(c.dau_a), r->a);
    std::copy(std::begin(c.dau_c), std::end(c.dau_c), r->c);
    r->auc = c.dau_auc;
    r->psw = (c.dau_psw & 0xfe10U) | (uint16_t(uint64_t(c.dau_a[0]) >> 32) & 15) |
             (uint16_t(uint64_t(c.dau_a[1]) >> 27) & 0x1e0);
    return 0;
}
extern "C" int dsp16_set_registers(DSP16 *d, const DSP16Registers *r) {
    if (!d || !r)
        return -1;
    auto &c = *d->m_core;
    c.xaau_pc = r->pc;
    c.xaau_pt = r->pt;
    c.xaau_pr = r->pr;
    c.xaau_pi = r->pi;
    c.xaau_i = r->i;
    c.xaau_extend_i();
    for (int i = 0; i < 4; ++i)
        c.yaau_r[i] = r->r[i] & c.yaau_mask;
    c.yaau_set_rb(r->rb);
    c.yaau_set_re(r->re);
    c.yaau_set_j(r->j);
    c.yaau_set_k(r->k);
    c.dau_x = r->x;
    c.dau_y = r->y;
    c.dau_p = r->p;
    std::copy(r->a, r->a + 2, c.dau_a);
    c.dau_extend_a<0>();
    c.dau_extend_a<1>();
    std::copy(r->c, r->c + 3, c.dau_c);
    c.dau_auc = r->auc & 127;
    c.dau_psw = r->psw;
    d->m_cache_mode = DSP16::cache::NONE;
    d->m_phase = DSP16::phase::PURGE;
    d->m_cache_ptr = 0;
    d->m_flags = DSP16::FLAGS_NONE;
    d->m_st_pcbase = r->pc;
    return 0;
}
extern "C" int dsp16_get_status(const DSP16 *d, DSP16Status *s) {
    if (!d || !s)
        return -1;
    std::memset(s, 0, sizeof(*s));
    dsp16_get_registers(d, &s->registers);
    s->cycles = d->m_core->cycles;
    s->instructions = d->instructions;
    s->warnings = d->warnings;
    s->faults = d->faults;
    s->instruction_pc = d->m_st_pcbase;
    s->sioc = d->m_sio_sioc;
    s->pioc = d->m_pio_pioc;
    s->serial_buffer = d->m_sio_obuf;
    s->parallel_input = d->m_pio_pdx_in;
    s->parallel_output = d->m_pio_pdx_out;
    s->phase = u8(d->m_phase);
    s->cache_mode = u8(d->m_cache_mode);
    s->cache_pointer = d->m_cache_ptr;
    s->cache_limit = d->m_cache_limit;
    s->cache_iterations = d->m_cache_iterations;
    s->halted = d->halted;
    u8 p[] = {d->m_iack_out,
              u8(d->sio_ick_active() ? d->m_sio_clk : 1),
              u8(d->sio_ild_active() ? d->m_sio_ld : 1),
              d->m_do_out,
              u8(d->sio_ock_active() ? d->m_sio_clk : 1),
              u8(d->sio_old_active() ? d->m_sio_ld : 1),
              d->m_ose_out,
              d->m_psel_out,
              d->m_pids_out,
              d->m_pods_out};
    std::copy(p, p + 10, s->pins);
    return 0;
}
