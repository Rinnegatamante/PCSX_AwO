// SPDX-License-Identifier: BSD-3-Clause
// Independent arithmetic expectations and deterministic pin/bus protocol traces.
#include "dsp16.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <vector>
struct Bus {
    DSP16 *d = nullptr;
    std::vector<std::tuple<unsigned, int>> pins;
    std::vector<std::pair<unsigned, uint16_t>> writes;
    std::vector<uint16_t> ext;
    std::vector<int> serial_bits;
    unsigned reads = 0, faults = 0, warns = 0;
    static void pin(void *p, unsigned n, int x) {
        auto *b = static_cast<Bus *>(p);
        b->pins.emplace_back(n, x);
        if (b->d && n == DSP16_PIN_OCK && x) {
            DSP16Status status{};
            dsp16_get_status(b->d, &status);
            if (!status.pins[DSP16_PIN_OSE])
                b->serial_bits.push_back(status.pins[DSP16_PIN_DO]);
        }
    }
    static uint16_t read(void *p, unsigned s) {
        auto *b = static_cast<Bus *>(p);
        ++b->reads;
        return 0x9000 + s;
    }
    static void write(void *p, unsigned s, uint16_t v) { static_cast<Bus *>(p)->writes.emplace_back(s, v); }
    static uint16_t external(void *p, uint16_t a) {
        auto *b = static_cast<Bus *>(p);
        b->ext.push_back(a);
        return 0xBEEF;
    }
    static void diag(void *p, int severity, uint16_t, const char *) {
        auto *b = static_cast<Bus *>(p);
        severity ? ++b->faults : ++b->warns;
    }
};
struct Fixture {
    std::array<uint16_t, 0x1000> rom{};
    Bus bus;
    DSP16 *d = nullptr;
    void create(int model = DSP16_MODEL_16A, bool ext = true) {
        DSP16Config c{};
        c.model = model;
        c.rom = rom.data();
        c.rom_words = model ? 0x1000 : 0x800;
        c.opaque = &bus;
        c.external_read = ext ? Bus::external : nullptr;
        c.pio_read = Bus::read;
        c.pio_write = Bus::write;
        c.pin_write = Bus::pin;
        c.diagnostic = Bus::diag;
        d = dsp16_create(&c);
        bus.d = d;
        assert(d);
    }
    ~Fixture() { dsp16_destroy(d); }
    DSP16Status status() {
        DSP16Status s{};
        assert(!dsp16_get_status(d, &s));
        return s;
    }
    DSP16Registers regs() { return status().registers; }
    void set(const DSP16Registers &r) { assert(!dsp16_set_registers(d, &r)); }
    void run(unsigned n) {
        assert(dsp16_run_cycles(d, n) == n);
        assert(!status().halted);
    }
};
static int64_t signed36(int64_t x) {
    const int64_t modulus = INT64_C(1) << 36;
    uint64_t v = uint64_t(x) & uint64_t(modulus - 1);
    return v & (uint64_t(1) << 35) ? int64_t(v) - modulus : int64_t(v);
}
static uint16_t flags(int64_t raw) {
    int64_t val = signed36(raw);
    uint16_t f = val < 0 ? 0x8000 : 0;
    if (!val)
        f |= 0x4000;
    if (raw != val)
        f |= 0x2000;
    if (val != int32_t(uint32_t(val)))
        f |= 0x1000;
    return f;
}
static int64_t aligned(int32_t p, int align) {
    if (!align)
        return p;
    if (align == 1)
        return p >> 2;
    return int64_t(p) * 4;
}
static uint64_t seed = 0xd516cafe12345678ULL;
static uint64_t random64() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}
static unsigned arithmetic() {
    unsigned count = 0;
    for (int f = 0; f < 16; ++f)
        for (int src = 0; src < 2; ++src)
            for (int dst = 0; dst < 2; ++dst)
                for (int sample = 0; sample < 64; ++sample) {
                    Fixture t;
                    t.rom[0] = 0x3000 | (dst << 10) | (src << 9) | (f << 5);
                    t.create();
                    DSP16Registers r{};
                    r.a[0] = signed36(random64());
                    r.a[1] = signed36(random64());
                    r.y = int32_t(random64());
                    r.x = int16_t(random64());
                    r.p = int32_t(random64());
                    r.auc = sample % 3;
                    r.psw = 0xA000;
                    int64_t s = r.a[src], p = aligned(r.p, r.auc), raw = r.a[dst];
                    bool change = true, flagchange = true;
                    int32_t product = r.p;
                    switch (f) {
                    case 0:
                        raw = p;
                        product = int32_t(r.x) * (r.y >> 16);
                        break;
                    case 1:
                        raw = s + p;
                        product = int32_t(r.x) * (r.y >> 16);
                        break;
                    case 2:
                        change = flagchange = false;
                        product = int32_t(r.x) * (r.y >> 16);
                        break;
                    case 3:
                        raw = s - p;
                        product = int32_t(r.x) * (r.y >> 16);
                        break;
                    case 4:
                        raw = p;
                        break;
                    case 5:
                        raw = s + p;
                        break;
                    case 6:
                        change = flagchange = false;
                        break;
                    case 7:
                        raw = s - p;
                        break;
                    case 8:
                        raw = s | r.y;
                        break;
                    case 9:
                        raw = s ^ r.y;
                        break;
                    case 10:
                        raw = s & r.y;
                        change = false;
                        break;
                    case 11:
                        raw = s - r.y;
                        change = false;
                        break;
                    case 12:
                        raw = r.y;
                        break;
                    case 13:
                        raw = s + r.y;
                        break;
                    case 14:
                        raw = s & r.y;
                        break;
                    case 15:
                        raw = s - r.y;
                        break;
                    }
                    t.set(r);
                    t.run(2);
                    auto out = t.regs();
                    assert(out.p == product);
                    assert(out.a[dst] == (change ? signed36(raw) : r.a[dst]));
                    assert(out.a[!dst] == r.a[!dst]);
                    assert((out.psw & 0xf000) == (flagchange ? flags(raw) : 0xa000));
                    ++count;
                }
    for (int f = 0; f < 16; ++f)
        if (f != 10)
            for (int src = 0; src < 2; ++src)
                for (int dst = 0; dst < 2; ++dst)
                    for (int sample = 0; sample < 32; ++sample) {
                        Fixture t;
                        t.rom[0] = 0x980e | (dst << 10) | (src << 9) | (f << 5);
                        t.create();
                        DSP16Registers r{};
                        r.a[0] = signed36(random64());
                        r.a[1] = signed36(random64());
                        r.y = int32_t(random64());
                        r.p = int32_t(random64());
                        r.auc = sample % 3;
                        r.auc |= (sample & 1) ? 0x30 : 0;
                        int64_t s = r.a[src], raw = 0;
                        switch (f) {
                        case 0:
                            raw = s >> 1;
                            break;
                        case 1:
                            raw = int32_t(uint32_t(s) << 1);
                            break;
                        case 2:
                            raw = s >> 4;
                            break;
                        case 3:
                            raw = int32_t(uint32_t(s) << 4);
                            break;
                        case 4:
                            raw = s >> 8;
                            break;
                        case 5:
                            raw = int32_t(uint32_t(s) << 8);
                            break;
                        case 6:
                            raw = s >> 16;
                            break;
                        case 7:
                            raw = int32_t(uint32_t(s) << 16);
                            break;
                        case 8:
                            raw = aligned(r.p, r.auc & 3);
                            break;
                        case 9:
                            raw = int64_t(int32_t(uint32_t(s) & 0xffff0000)) +
                                  65536; /* Upstream assigns destination before its low-half preservation
                                            read. */
                            break;
                        case 11:
                            raw = (s + (s < 0 ? 32767 : 32768)) & ~INT64_C(65535);
                            break;
                        case 12:
                            raw = r.y;
                            break;
                        case 13:
                            raw = s + 1;
                            break;
                        case 14:
                            raw = s;
                            break;
                        case 15:
                            raw = -s;
                            break;
                        }
                        t.set(r);
                        t.run(2);
                        auto out = t.regs();
                        if (out.a[dst] != signed36(raw)) {
                            fprintf(stderr, "F2 %x src%d dst%d sample%d got%llx exp%llx\n", f, src, dst,
                                    sample, (long long)out.a[dst], (long long)signed36(raw));
                            abort();
                        }
                        assert((out.psw & 0xf000) == flags(raw));
                        ++count;
                    }
    return count;
}
static void pipeline_memory() {
    Fixture t;
    t.rom[0] = 0x5100;
    t.rom[1] = 0x1234; // x = N, two cycles
    t.rom[2] = 0x6101; // *r0++ = x, two cycles
    t.rom[3] = 0x1807; // r0 = 7, one cycle
    t.rom[4] = 0x7900; // x = *r0, two cycles
    t.create();
    t.run(1);
    assert(t.regs().pc == 0);
    t.run(1);
    assert(t.regs().x == 0);
    t.run(1);
    assert(t.regs().x == 0x1234);
    assert(t.regs().pc == 2);
    t.run(1);
    assert(dsp16_ram_read(t.d, 0) == 0);
    t.run(1);
    assert(dsp16_ram_read(t.d, 0) == 0x1234);
    assert(t.regs().r[0] == 1);
    t.run(1);
    assert(t.regs().r[0] == 7);
    dsp16_ram_write(t.d, 7, 0xabba);
    t.run(1);
    assert(uint16_t(t.regs().x) == 0xabba);
    t.run(1);
    // Circular pointer increment and Z exchange postmodify.
    Fixture z;
    z.rom[0] = 0xB0C1;
    z.rom[1] = 0x6900;
    z.create();
    DSP16Registers r{};
    r.r[0] = 4;
    r.rb = 2;
    r.re = 4;
    r.x = 0x2222;
    z.set(r);
    dsp16_ram_write(z.d, 4, 0x1111);
    z.run(2);
    assert(z.regs().r[0] == 2);
    assert(z.regs().x == 0x1111); // x = Y with NOP F1
    z.run(2);
    assert(z.regs().r[0] == 3);
    assert(dsp16_ram_read(z.d, 2) == 0x1111); // Z : x exchanges the input with x and increments r0.
    // DSP16 pointer width/sign and mirrored RAM differs from DSP16A.
    Fixture small;
    small.create(DSP16_MODEL_16);
    assert(!dsp16_write_register(small.d, 0, 0xffff));
    assert(small.regs().r[0] == 511);
    assert(!dsp16_write_register(small.d, 4, 0x100));
    assert(small.regs().j == -256);
    dsp16_ram_write(small.d, 0x201, 0x8877);
    assert(dsp16_ram_read(small.d, 1) == 0x8877);
    dsp16_reset(t.d);
    assert(t.regs().x == int16_t(0xabba));
    assert(dsp16_ram_read(t.d, 0) == 0x1234);
    assert(t.regs().pc == 0);
}
static void branch_cache() {
    Fixture t;
    t.rom[0] = 0x8004;
    t.rom[1] = 0x1803;
    t.rom[4] = 0x1a05;
    t.rom[5] = 0xc000;
    t.create();
    t.run(2);
    assert(t.regs().pr == 1 && t.regs().pc == 4);
    t.run(1);
    t.run(1);
    assert(t.regs().r[1] == 5);
    t.run(1);
    assert(t.regs().pc == 1);
    t.run(2);
    assert(t.regs().r[0] == 3);
    Fixture c;
    c.rom[0] = 0x7085; // do 5, one instruction
    c.rom[1] = 0x99ae; // if true a0 = a0 + 1
    c.rom[2] = 0x1809;
    c.create();
    c.run(1);
    c.run(1);
    assert(c.status().cache_mode == 1);
    c.run(1);
    assert(c.regs().a[0] == 1);
    c.run(1);
    for (int i = 2; i <= 5; ++i) {
        c.run(1);
        assert(c.regs().a[0] == i);
    }
    assert(c.status().cache_mode == 0);
    c.run(1);
    assert(c.regs().r[0] == 9);
    Fixture redo;
    redo.rom[0] = 0x7082;
    redo.rom[1] = 0x99ae;
    redo.rom[2] = 0x7003;
    redo.rom[3] = 0x180a;
    redo.create();
    redo.run(11);
    assert(redo.regs().a[0] == 5 && redo.regs().r[0] == 10);
    // XAAU PC and PT additions wrap inside 4Ki-word page.
    Fixture page;
    page.rom[0xfff] = 0x1807;
    page.create();
    DSP16Registers r{};
    r.pc = 0x0fff;
    page.set(r);
    page.run(2);
    assert(page.regs().pc == 0 && page.regs().r[0] == 7);
    Fixture romload;
    romload.rom[0] = 0xc8c0;
    romload.create();
    r = {};
    r.pt = 0x1234;
    r.a[0] = 0x12345678;
    romload.set(r);
    romload.run(3);
    assert(uint16_t(romload.regs().x) == 0xbeef);
    assert(romload.regs().pt == 0x1235);
    assert(romload.regs().y == 0x12345678);
    assert(romload.bus.ext == std::vector<uint16_t>{0x1234});
}
static void pio_serial_irq() {
    Fixture t;
    t.create();
    dsp16_host_pio_write(t.d, 0x4567);
    assert(t.status().pioc & 4);
    uint16_t v;
    assert(!dsp16_read_register(t.d, 0x1d, &v));
    assert(v == 0x4567 && !(t.status().pioc & 4));
    assert(!dsp16_write_register(t.d, 0x1e, 0x6789));
    assert(dsp16_host_pio_read(t.d) == 0x6789 && (t.status().pioc & 2));
    // Active strobes have exactly the configured 1..4-cycle duration.
    for (int duration = 1; duration <= 4; ++duration) {
        Fixture a;
        a.create();
        assert(!dsp16_write_register(a.d, 0x1c, 0x1800 | ((duration - 1) << 13)));
        assert(!dsp16_write_register(a.d, 0x1e, 0xABCD));
        assert(a.status().pins[DSP16_PIN_PODS] == 0);
        a.run(duration);
        assert(a.bus.writes.empty());
        a.run(1);
        assert(a.bus.writes.size() == 1 && a.bus.writes[0] == std::make_pair(1U, uint16_t(0xabcd)));
        assert(!dsp16_read_register(a.d, 0x1d, &v));
        a.run(duration + 1);
        assert(a.bus.reads == 1);
        assert(a.status().parallel_input == 0x9000);
    }
    // Passive external serial clock, both bit orders and 8/16-bit lengths.
    for (int msb = 0; msb <= 1; ++msb)
        for (int eight = 0; eight <= 1; ++eight) {
            Fixture s;
            s.create();
            assert(!dsp16_write_register(s.d, 0x18, (msb ? 0x40 : 0) | (eight ? 2 : 0)));
            assert(!dsp16_write_register(s.d, 0x1a, 0xa35c));
            dsp16_set_input(s.d, DSP16_INPUT_OLD, 1);
            dsp16_set_input(s.d, DSP16_INPUT_OCK, 0);
            int n = eight ? 8 : 16;
            for (int i = 0; i < n; ++i) {
                assert(!dsp16_set_input(s.d, DSP16_INPUT_OCK, 1));
                int expected = msb ? ((0xa35c >> (15 - i)) & 1) : ((0xa35c >> i) & 1);
                assert(s.status().pins[DSP16_PIN_DO] == expected);
                assert(!s.status().pins[DSP16_PIN_OSE]);
                assert(!dsp16_set_input(s.d, DSP16_INPUT_OCK, 0));
            }
            assert(!dsp16_set_input(s.d, DSP16_INPUT_OCK, 1));
            assert(s.status().pins[DSP16_PIN_OSE]);
            assert(s.status().pioc & 8);
        }
    // 60MHz/12 clock selects half-period 3 machine cycles.
    Fixture clk;
    clk.create();
    dsp16_write_register(clk.d, 0x18, 0x88);
    clk.bus.pins.clear();
    clk.run(13);
    std::vector<int> edges;
    for (auto [p, x] : clk.bus.pins)
        if (p == DSP16_PIN_OCK)
            edges.push_back(x);
    assert((edges == std::vector<int>{0, 1, 0, 1, 0}));
    Fixture irq;
    irq.rom[1] = 0x000a;
    irq.rom[10] = 0x1a55;
    irq.rom[11] = 0xc100;
    irq.rom[20] = 0x1801;
    irq.rom[21] = 0x1802;
    irq.rom[22] = 0x1803;
    irq.create();
    DSP16Registers r{};
    r.pc = 20;
    irq.set(r);
    dsp16_write_register(irq.d, 0x1c, 0x20);
    dsp16_set_input(irq.d, DSP16_INPUT_INT, 1);
    irq.run(2);
    assert(irq.regs().r[0] == 1);
    irq.run(1);
    assert(irq.regs().pc == 1);
    irq.run(1);
    irq.run(1);
    assert(!irq.status().pins[DSP16_PIN_IACK]);
    dsp16_set_input(irq.d, DSP16_INPUT_INT, 0);
    irq.run(2);
    assert(irq.regs().r[1] == 0x55);
    irq.run(1);
    irq.run(3);
    assert(irq.status().pins[DSP16_PIN_IACK]);
    assert(irq.regs().r[0] >= 2);
}
static void synthetic_rom_fixture() {
    Fixture f;
    // Genuine instruction-encoded peripheral configuration and data transfers.
    const uint16_t program[] = {
        0x51c0, 0x1800, // PIOC: active input/output strobes
        0x51d0, 0x0040, // PDX0 -> external PIO callback
        0x5180, 0x02e8, // SIOC: active OLD/OCK, CKI/12, MSB first
        0x51a0, 0xa35c, // SDX output buffer
        0x5080, 0x1000, // PT external sample ROM address
        0xc8c0,         // y=a0; x=*pt++, no ALU operation
        0x5000, 0x0064, // r0=100
        0x5100, 0x55aa, // x=0x55aa
        0x6100,         // RAM[r0]=x
        0x0010          // goto 16; peripherals continue cycling
    };
    std::copy(std::begin(program), std::end(program), f.rom.begin());
    f.create();
    f.run(128);
    assert((f.bus.writes == std::vector<std::pair<unsigned, uint16_t>>({{0, 0x0040}})));
    assert(f.bus.ext == std::vector<uint16_t>{0x1000});
    assert(dsp16_ram_read(f.d, 100) == 0x55aa);
    assert(f.regs().pt == 0x1001 && f.status().instruction_pc == 16);
    assert(f.bus.serial_bits.size() == 16);
    uint16_t recovered = 0;
    for (int bit : f.bus.serial_bits)
        recovered = (recovered << 1) | bit;
    assert(recovered == 0xa35c && (f.status().pioc & 8));
    assert(!f.status().warnings && !f.status().faults);
}
static unsigned opcode_smoke() {
    unsigned count = 0;
    // One legal encoding from every defined top-level family, both DSP models.
    const uint16_t ops[32] = {0x0000, 0x0800, 0x1001, 0x1801, 0x20c0, 0x28c0, 0x30c0, 0x38c0,
                              0x4000, 0x4800, 0x5000, 0x5800, 0x6000, 0x6800, 0x7082, 0x7800,
                              0x8000, 0x8800, 0x900e, 0x980e, 0xa0c0, 0xa8c0, 0xb0c0, 0xb8c0,
                              0xc000, 0xc8c0, 0xd00e, 0xd8c0, 0xe0c0, 0xe8c0, 0xf000, 0xf8c0};
    for (int model = 0; model < 2; ++model)
        for (int family = 0; family < 32; ++family)
            if (family != 30) {
                Fixture f;
                f.rom[0] = ops[family];
                f.rom[1] = family == 26 ? 0x0002 : 0x99ae;
                f.create(model);
                if (dsp16_run_cycles(f.d, 3) != 3) {
                    fprintf(stderr, "smoke family%x model%d\n", family, model);
                    abort();
                }
                ++count;
            }
    // Conditional branch prefix, false and true, and eligibility fault.
    for (int truth = 0; truth < 2; ++truth) {
        Fixture p;
        p.rom[0] = truth ? 0xd00e : 0xd00f;
        p.rom[1] = 0x0007;
        p.create();
        p.run(3);
        assert(p.regs().pc == (truth ? 7 : 2));
    }
    Fixture bad;
    bad.rom[0] = 0xd00e;
    bad.rom[1] = 0x1801;
    bad.create();
    assert(dsp16_run_cycles(bad.d, 3) == 2 && bad.status().halted);
    // Cached two-cycle stores still write during OP2, with all iterations.
    Fixture store;
    store.rom[0] = 0x7083;
    store.rom[1] = 0x6101;
    store.rom[2] = 0x1807;
    store.create();
    DSP16Registers r{};
    r.x = 0x4321;
    store.set(r);
    store.run(8);
    assert(dsp16_ram_read(store.d, 0) == 0x4321 && dsp16_ram_read(store.d, 1) == 0x4321 &&
           dsp16_ram_read(store.d, 2) == 0x4321);
    assert(store.regs().r[0] == 3);
    // ALU saturation is used on accumulator transfers unless AUC disables it.
    for (int disable = 0; disable < 2; ++disable) {
        Fixture sat;
        sat.rom[0] = 0x4900;
        sat.create();
        r = {};
        r.a[0] = INT64_C(0x180000000);
        r.auc = disable ? 4 : 0;
        sat.set(r);
        sat.run(3);
        assert(uint16_t(sat.regs().x) == (disable ? 0x8000 : 0x7fff));
        r.a[0] = -INT64_C(0x180000001);
        sat.set(r);
        sat.run(3);
        assert(uint16_t(sat.regs().x) == (disable ? 0x7fff : 0x8000));
    }
    // DSP16 internal ROM mirror and explicit EXM external selection.
    Fixture mirror;
    mirror.rom[3] = 0x1805;
    mirror.create(DSP16_MODEL_16);
    r = {};
    r.pc = 0x803;
    mirror.set(r);
    mirror.run(2);
    assert(mirror.regs().r[0] == 5 && mirror.bus.ext.empty());
    dsp16_set_input(mirror.d, DSP16_INPUT_EXM, 0);
    mirror.set(r);
    mirror.run(1);
    assert(mirror.bus.ext == std::vector<uint16_t>{0x803});
    return count;
}
static void diagnostics_slicing() {
    for (uint16_t op :
         {uint16_t(0xf000), uint16_t(0x994e), uint16_t(0x9808), uint16_t(0x5101), uint16_t(0x7001)}) {
        Fixture f;
        f.rom[0] = op;
        f.create();
        assert(dsp16_run_cycles(f.d, 20) < 20);
        assert(f.status().halted && f.status().faults == 1 && f.bus.faults == 1);
        assert(dsp16_run_cycles(f.d, 20) == 0);
        dsp16_reset(f.d);
        assert(!f.status().halted);
    }
    Fixture sc;
    sc.create();
    assert(dsp16_write_register(sc.d, 0x1c, 0x400) == -1 && sc.status().halted);
    Fixture fail;
    fail.create(DSP16_MODEL_16A, false);
    dsp16_set_input(fail.d, DSP16_INPUT_EXM, 0);
    assert(dsp16_run_cycles(fail.d, 1) == 0 && fail.status().halted);
    Fixture serial;
    serial.create();
    uint16_t value;
    assert(dsp16_read_register(serial.d, 0x1a, &value) == -1);
    Fixture rx;
    rx.create();
    dsp16_set_input(rx.d, DSP16_INPUT_ICK, 0);
    dsp16_set_input(rx.d, DSP16_INPUT_ILD, 1);
    assert(!dsp16_set_input(rx.d, DSP16_INPUT_ICK, 1) && !rx.status().halted);
    assert(rx.status().warnings == 1);
    dsp16_set_input(rx.d, DSP16_INPUT_ICK, 0);
    dsp16_set_input(rx.d, DSP16_INPUT_ICK, 1);
    assert(rx.status().warnings == 1);
    Fixture a, b;
    for (unsigned i = 0; i < 0x1000; ++i)
        a.rom[i] = b.rom[i] = 0x99ae;
    a.create();
    b.create();
    a.run(10000);
    for (int i = 0; i < 10000; ++i)
        b.run(1);
    auto sa = a.status(), sb = b.status();
    assert(!std::memcmp(&sa, &sb, sizeof sa));
    assert(sa.registers.a[0] == 9999 && sa.cycles == 10000);
    DSP16Config bad{};
    assert(!dsp16_create(&bad));
}
int main() {
    unsigned n = arithmetic();
    pipeline_memory();
    branch_cache();
    pio_serial_irq();
    synthetic_rom_fixture();
    unsigned smoke = opcode_smoke();
    diagnostics_slicing();
    printf("DSP16 tests PASS (%u independent DAU vectors, %u opcode-family/model cases, "
           "cache/bus/PIO/SIO/IRQ/slices)\n",
           n, smoke);
}
