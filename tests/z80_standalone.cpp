// Synthetic standalone Z80 traces; no board program/media substitutions.
#include "z80.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
struct Bus {
    uint8_t memory[65536] = {}, io[65536] = {}, opcode_bus[65536] = {};
    uint32_t vector = 0xff;
    unsigned acks = 0, retis = 0, refreshes = 0, nomreqs = 0, reads = 0, writes = 0,
             diagnostics = 0;
    uint16_t last_port = 0;
    uint8_t last_value = 0;
    int pins[3] = {};
    static uint8_t r(void *p, uint16_t a) {
        Bus &b = *(Bus *)p;
        b.reads++;
        return b.memory[a];
    }
    static uint8_t op(void *p, uint16_t a) { return ((Bus *)p)->opcode_bus[a]; }
    static void w(void *p, uint16_t a, uint8_t v) {
        Bus &b = *(Bus *)p;
        b.writes++;
        b.memory[a] = v;
    }
    static uint8_t in(void *p, uint16_t a) {
        Bus &b = *(Bus *)p;
        b.last_port = a;
        return b.io[a];
    }
    static void out(void *p, uint16_t a, uint8_t v) {
        Bus &b = *(Bus *)p;
        b.last_port = a;
        b.last_value = v;
        b.io[a] = v;
    }
    static uint32_t ack(void *p) {
        Bus &b = *(Bus *)p;
        b.acks++;
        return b.vector;
    }
    static void reti(void *p) { ((Bus *)p)->retis++; }
    static void pin(void *p, unsigned n, int v) { ((Bus *)p)->pins[n] = v; }
    static void refresh(void *p, uint16_t) { ((Bus *)p)->refreshes++; }
    static void nomreq(void *p, uint16_t) { ((Bus *)p)->nomreqs++; }
    static void diag(void *p, unsigned, uint16_t, uint32_t) { ((Bus *)p)->diagnostics++; }
    Z80Config config() {
        Z80Config c = {};
        c.opaque = this;
        c.memory_read = r;
        c.memory_write = w;
        c.io_read = in;
        c.io_write = out;
        c.irq_ack = ack;
        c.reti = reti;
        c.pin = pin;
        c.refresh = refresh;
        c.nomreq = nomreq;
        c.diagnostic = diag;
        return c;
    }
    Z80 *cpu() {
        auto c = config();
        auto d = z80_create(&c);
        assert(d);
        return d;
    }
    void put(uint16_t a, std::initializer_list<uint8_t> bytes) {
        for (auto b : bytes)
            memory[a++] = b;
    }
    void word(uint16_t a, uint16_t v) {
        memory[a] = v;
        memory[uint16_t(a + 1)] = v >> 8;
    }
};
static Z80Status status(Z80 *c) {
    Z80Status s;
    z80_get_status(c, &s);
    return s;
}
static uint64_t step(Z80 *c, uint64_t expected = 0) {
    auto n = z80_step_instruction(c);
    if (expected && n != expected)
        std::fprintf(stderr, "Step PC%x cycles%llu expected%llu phase%x\n", status(c).pc,
                     (unsigned long long)n, (unsigned long long)expected,
                     status(c).execution_phase);
    assert(!expected || n == expected);
    return n;
}
struct AluVector {
    unsigned prefix, opcode, a, operand, flags, result, result_flags, cycles;
};
struct WideVector {
    unsigned prefix, opcode, lhs, rhs, flags, result, result_flags, cycles;
};
#include "z80_vectors.inc"
static void vectors() {
    Bus b;
    auto c = b.cpu();
    for (const auto &v : alu_vectors) {
        z80_reset(c);
        z80_set_register(c, Z80_AF, (v.a << 8) | v.flags);
        z80_set_register(c, Z80_B, v.a);
        unsigned p = 0;
        if (v.prefix)
            b.memory[p++] = v.prefix;
        b.memory[p++] = v.opcode;
        b.memory[p] = v.operand;
        auto n = z80_step_instruction(c);
        auto result = z80_get_register(c, v.prefix ? Z80_B : Z80_A),
             flags = z80_get_register(c, Z80_F);
        if (n != v.cycles || result != v.result || flags != v.result_flags) {
            std::fprintf(
                stderr,
                "Vector prefix%x op%x A%x operand%x F%x got R%x F%x T%llu want R%x F%x T%u\n",
                v.prefix, v.opcode, v.a, v.operand, v.flags, result, flags, (unsigned long long)n,
                v.result, v.result_flags, v.cycles);
            assert(false);
        }
    }
    for (const auto &v : wide_vectors) {
        z80_reset(c);
        z80_set_register(c, Z80_HL, v.lhs);
        z80_set_register(c, Z80_BC, v.rhs);
        z80_set_register(c, Z80_F, v.flags);
        unsigned p = 0;
        if (v.prefix)
            b.memory[p++] = v.prefix;
        b.memory[p] = v.opcode;
        auto n = z80_step_instruction(c);
        assert(n == v.cycles && z80_get_register(c, Z80_HL) == v.result &&
               z80_get_register(c, Z80_F) == v.result_flags);
    }
    std::printf("Z80 %zu independent 16-bit arithmetic flags/timing vectors passed\n",
                sizeof(wide_vectors) / sizeof(wide_vectors[0]));
    z80_destroy(c);
    std::printf("Z80 %zu independent binary/INC-DEC/DAA/rotate flags and timing vectors passed\n",
                sizeof(alu_vectors) / sizeof(alu_vectors[0]));
}
static void smoke() {
    Bus b;
    auto c = b.cpu();
    unsigned count = 0;
    for (unsigned family = 0; family < 7; family++)
        for (unsigned opcode = 0; opcode < 256; opcode++) {
            z80_reset(c);
            z80_set_register(c, Z80_SP, 0x9000);
            z80_set_register(c, Z80_BC, 1);
            z80_set_register(c, Z80_HL, 0x8000);
            z80_set_register(c, Z80_DE, 0x8100);
            z80_set_register(c, Z80_IX, 0x8200);
            z80_set_register(c, Z80_IY, 0x8300);
            memset(b.memory, 0, 32);
            b.memory[0x8000] = 0x80;
            b.memory[0x8200] = 0x81;
            b.memory[0x8300] = 0x82;
            if (family < 5) {
                const uint8_t prefixes[] = {0, 0xcb, 0xed, 0xdd, 0xfd};
                unsigned p = 0;
                if (prefixes[family])
                    b.memory[p++] = prefixes[family];
                b.memory[p] = opcode;
            } else {
                b.memory[0] = family == 5 ? 0xdd : 0xfd;
                b.memory[1] = 0xcb;
                b.memory[2] = 0;
                b.memory[3] = opcode;
            }
            auto before = status(c);
            auto n = z80_step_instruction(c);
            auto after = status(c);
            assert(n > 0 && n < 100 && after.instructions == before.instructions + 1);
            count++;
        }
    z80_destroy(c);
    std::printf("Z80 %u base/CB/ED/DD/FD/DDCB/FDCB opcode dispatches passed\n", count);
}
static void basic() {
    Bus b;
    auto c = b.cpu();
    assert(status(c).ix == 0xffff && status(c).iy == 0xffff && (status(c).af & 64));
    b.put(0, {0x31, 0, 0x90, 0x3e, 0x12, 0x06, 0x34, 0x80, 0x32, 0, 0x80, 0x76});
    step(c, 10);
    step(c, 7);
    step(c, 7);
    step(c, 4);
    assert(z80_get_register(c, Z80_A) == 0x46);
    step(c, 13);
    assert(b.memory[0x8000] == 0x46);
    step(c, 4);
    assert(status(c).halt && b.pins[Z80_PIN_HALT]);
    auto p = status(c).pc;
    step(c, 4);
    assert(status(c).pc == p);
    assert(status(c).instructions == 6 && b.refreshes == 7);
    z80_set_register(c, Z80_BC, 0x1234);
    z80_set_register(c, Z80_AF2, 0x4567);
    z80_reset(c);
    assert(status(c).pc == 0 && !status(c).halt && status(c).bc == 0x1234 &&
           status(c).af2 == 0x4567);
    z80_destroy(c);
    std::puts("Z80 basic register/timing/HALT/refresh/reset lifecycle passed");
}
static void prefixes() {
    Bus b;
    auto c = b.cpu();
    b.put(0,
          {0xdd, 0x21, 0, 0x80, 0xfd, 0x21, 0, 0x81, 0xdd, 0x36, 0xfe, 0x81, 0xdd, 0xcb, 0xfe, 0x00,
           0xfd, 0x36, 1, 0x80, 0xfd, 0xcb, 1, 0x7e, 0xdd, 0xfd, 0x24, 0xed, 0x44, 0x76});
    step(c, 14);
    step(c, 14);
    step(c, 19);
    step(c, 23);
    assert(b.memory[0x7ffe] == 3 && z80_get_register(c, Z80_B) == 3);
    step(c, 19);
    step(c, 20);
    assert(!(z80_get_register(c, Z80_F) & 64));
    step(c, 12);
    assert(status(c).iy == 0x8200 && status(c).ix == 0x8000);
    z80_set_register(c, Z80_A, 1);
    step(c, 8);
    assert(z80_get_register(c, Z80_A) == 0xff);
    step(c, 4);
    assert(status(c).r == 18);
    z80_destroy(c);
    Bus d;
    auto e = d.cpu();
    d.put(0, {0x21, 0, 0x80, 0xcb, 0x36, 0xcb, 0x7e, 0xed, 0, 0x76});
    d.memory[0x8000] = 0x80;
    step(e, 10);
    step(e, 15);
    assert(d.memory[0x8000] == 1);
    step(e, 12);
    assert(z80_get_register(e, Z80_F) & 64);
    step(e, 8);
    assert(status(e).undocumented_opcodes == 1);
    z80_destroy(e);
    std::puts("Z80 CB/DD/FD/ED/DDCB/FDCB/repeated prefixes/undocumented NOP passed");
}
static void blocks_io() {
    Bus b;
    auto c = b.cpu();
    b.put(0, {0xed, 0xb0, 0xed, 0xb8, 0xd3, 0x34, 0xdb, 0x56, 0xed, 0x78, 0xed, 0x79, 0x76});
    z80_set_register(c, Z80_HL, 0x8000);
    z80_set_register(c, Z80_DE, 0x8100);
    z80_set_register(c, Z80_BC, 3);
    b.put(0x8000, {0x12, 0x34, 0x56});
    step(c, 21);
    step(c, 21);
    step(c, 16);
    assert(!memcmp(b.memory + 0x8000, b.memory + 0x8100, 3) && status(c).hl == 0x8003 &&
           status(c).de == 0x8103 && !status(c).bc);
    z80_set_register(c, Z80_HL, 0x8002);
    z80_set_register(c, Z80_DE, 0x8202);
    z80_set_register(c, Z80_BC, 3);
    step(c, 21);
    step(c, 21);
    step(c, 16);
    assert(!memcmp(b.memory + 0x8000, b.memory + 0x8200, 3));
    z80_set_register(c, Z80_A, 0x12);
    step(c, 11);
    assert(b.last_port == 0x1234 && b.last_value == 0x12);
    b.io[0x1256] = 0x55;
    step(c, 11);
    assert(z80_get_register(c, Z80_A) == 0x55 && b.last_port == 0x1256);
    z80_set_register(c, Z80_BC, 0xabcd);
    b.io[0xabcd] = 0x66;
    step(c, 12);
    assert(z80_get_register(c, Z80_A) == 0x66);
    step(c, 12);
    assert(b.last_port == 0xabcd && b.last_value == 0x66);
    z80_destroy(c);
    std::puts("Z80 LDIR/LDDR repeated timing/full 16-bit port I/O passed");
}
static void interrupts() {
    for (unsigned mode = 0; mode < 3; mode++) {
        Bus b;
        auto c = b.cpu();
        b.put(0, {0xfb, 0, 0x76});
        b.put(0x38, {0, 0xed, 0x4d});
        b.put(0x1234, {0, 0xed, 0x4d});
        z80_set_register(c, Z80_SP, 0x9000);
        z80_set_register(c, Z80_IM, mode);
        z80_set_register(c, Z80_I, 0x80);
        b.vector = mode == 2 ? 0xff : 0xff;
        b.word(0x80ff, 0x1234);
        z80_irq(c, 1);
        step(c, 4);
        assert(status(c).iff1 && b.acks == 0);
        step(c, 4);
        assert(b.acks == 0); // EI delays one complete instruction.
        step(c, mode == 2 ? 23 : 17);
        assert(b.acks == 1 && status(c).sp == 0x8ffe && b.memory[0x8ffe] == 2);
        assert(status(c).pc == (mode == 2 ? 0x1235 : 0x39));
        z80_irq(c, 0);
        step(c, 14);
        assert(status(c).pc == 2 && status(c).sp == 0x9000 && b.retis == 1);
        z80_destroy(c);
    }
    Bus b;
    auto c = b.cpu();
    b.put(0, {0xfb, 0, 0x76});
    b.put(0x66, {0, 0xed, 0x45});
    z80_set_register(c, Z80_SP, 0x9000);
    z80_nmi(c, 1);
    step(c, 4);
    assert(!status(c).nmis);
    z80_nmi(c, 0);
    step(c, 4);
    step(c, 4);
    assert(status(c).halt);
    z80_nmi(c, 1);
    step(c, 15);
    assert(status(c).pc == 0x67 && !status(c).iff1 && status(c).iff2 && !status(c).halt &&
           status(c).nmis == 1);
    step(c, 14);
    assert(status(c).pc == 3 && status(c).iff1 && status(c).sp == 0x9000);
    step(c, 4);
    assert(status(c).nmis == 1); // Held NMI does not retrigger.
    z80_destroy(c);
    Bus d;
    auto e = d.cpu();
    d.put(0, {0xfb, 0});
    z80_set_register(e, Z80_SP, 0x9000);
    d.vector = 0xcd4567;
    d.put(0x4567, {0});
    z80_irq(e, 1);
    step(e, 4);
    step(e, 4);
    step(e, 23);
    assert(status(e).pc == 0x4568 && status(e).sp == 0x8ffe);
    z80_destroy(e);
    std::puts("Z80 IM0/IM1/odd-vector IM2/EI delay/NMI edge/RETI/RETN passed");
}
static void extra_traces() {
    Bus b;
    auto c = b.cpu();
    b.put(0, {0xed, 0xb1, 0xed, 0xb2, 0xed, 0xb3, 0xfd, 0xcb, 0, 0x7e, 0xed, 0x67, 0xed, 0x6f});
    z80_set_register(c, Z80_A, 0x34);
    z80_set_register(c, Z80_HL, 0x8000);
    z80_set_register(c, Z80_BC, 3);
    b.put(0x8000, {0x12, 0x34, 0x56});
    step(c, 21);
    step(c, 16);
    assert(status(c).hl == 0x8002 && status(c).bc == 1 && (status(c).af & 64));
    z80_set_register(c, Z80_BC, 0x0210);
    z80_set_register(c, Z80_HL, 0x8100);
    b.io[0x0210] = 0x12;
    b.io[0x0110] = 0x34;
    step(c, 21);
    step(c, 16);
    assert(b.memory[0x8100] == 0x12 && b.memory[0x8101] == 0x34 && status(c).hl == 0x8102 &&
           status(c).bc == 0x0010);
    z80_set_register(c, Z80_BC, 0x0210);
    z80_set_register(c, Z80_HL, 0x8100);
    step(c, 21);
    step(c, 16);
    assert(b.io[0x0110] == 0x12 && b.io[0x0010] == 0x34 && status(c).bc == 0x0010);
    z80_set_register(c, Z80_IY, 0x2800);
    z80_set_register(c, Z80_F, 1);
    b.memory[0x2800] = 0x80;
    step(c, 20);
    assert(z80_get_register(c, Z80_F) == 0xb9);
    z80_set_register(c, Z80_HL, 0x8200);
    z80_set_register(c, Z80_A, 0x12);
    b.memory[0x8200] = 0x34;
    step(c, 18);
    assert(z80_get_register(c, Z80_A) == 0x14 && b.memory[0x8200] == 0x23);
    step(c, 18);
    assert(z80_get_register(c, Z80_A) == 0x12 && b.memory[0x8200] == 0x34);
    z80_destroy(c);
    Bus encrypted;
    auto cfg = encrypted.config();
    cfg.opcode_read = Bus::op;
    auto d = z80_create(&cfg);
    assert(d);
    encrypted.opcode_bus[0] = 0x3e;
    encrypted.memory[1] = 0x55;
    encrypted.opcode_bus[1] = 0xff;
    step(d, 7);
    assert(z80_get_register(d, Z80_A) == 0x55);
    z80_destroy(d);
    Bus im0;
    auto e = im0.cpu();
    z80_set_register(e, Z80_IFF1, 1);
    im0.vector = 0x12;
    z80_irq(e, 1);
    step(e, 6);
    assert(status(e).unsupported_operations == 1 && im0.diagnostics == 1);
    z80_destroy(e);
    std::puts("Z80 CPIR/INIR/OTIR/indexed BIT flags/RRD-RLD/decrypted fetch/IM0 diagnostic passed");
}
static void scheduler() {
    Bus a, b;
    auto c = a.cpu(), d = b.cpu();
    a.put(0, {0x3c, 0x18, 0xfd});
    b.put(0, {0x3c, 0x18, 0xfd});
    for (unsigned i = 0; i < 1000; i++)
        z80_run_cycles(c, 1);
    z80_run_cycles(d, 1000);
    auto x = status(c), y = status(d);
    assert(x.pc == y.pc && x.af == y.af && x.r == y.r && x.execution_phase == y.execution_phase &&
           x.cycles == y.cycles && x.instructions == y.instructions);
    auto before = status(c);
    z80_wait(c, 1);
    z80_run_cycles(c, 100);
    auto after = status(c);
    assert(before.pc == after.pc && before.instructions == after.instructions &&
           after.cycles >= before.cycles + 97);
    assert(!z80_step_instruction(c));
    z80_wait(c, 0);
    z80_busreq(c, 1);
    z80_run_cycles(c, 20);
    assert(status(c).busack && a.pins[Z80_PIN_BUSACK]);
    auto p = status(c).pc;
    z80_run_cycles(c, 20);
    assert(status(c).pc == p);
    z80_busreq(c, 0);
    z80_run_cycles(c, 20);
    assert(!status(c).busack);
    z80_destroy(c);
    z80_destroy(d);
    Bus prefixes;
    auto e = prefixes.cpu();
    memset(prefixes.memory, 0xdd, sizeof(prefixes.memory));
    z80_run_cycles(e, 10000);
    assert(status(e).cycles == 10000 && !status(e).instructions);
    z80_destroy(e);
    std::puts(
        "Z80 bus-step cycle debt/scheduler slices/WAIT/BUSREQ/unending prefix yielding passed");
}
int main() {
    vectors();
    smoke();
    basic();
    prefixes();
    blocks_io();
    interrupts();
    extra_traces();
    scheduler();
    return 0;
}
