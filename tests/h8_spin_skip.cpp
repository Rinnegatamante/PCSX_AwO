// SPDX-License-Identifier: BSD-3-Clause
// The exact idle-loop skip must be invisible: a CPU with peek8/account_reads
// and one without them stay identical after every h8_run_until call, while
// external IRQ edges and ADC conversions (internal events) interrupt the spin.
#include "../libpcsxcore/arcade/h8.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

enum { FLAG = 0x8000 };
struct Bus {
    H8CPU *cpu = nullptr;
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x10000, 0);
    uint64_t flag_reads = 0, bus_reads = 0;
};
static uint8_t read8(void *p, uint32_t a) {
    Bus &b = *static_cast<Bus *>(p);
    if ((a & 0xffff) == FLAG) { b.flag_reads++; b.bus_reads++; }
    return b.memory[a & 0xffff];
}
static void write8(void *p, uint32_t a, uint8_t v) { static_cast<Bus *>(p)->memory[a & 0xffff] = v; }
static uint16_t adc_read(void *, unsigned channel) { return 0x100 + channel; }
static int peek8(void *p, uint32_t a, uint8_t *v) {
    *v = static_cast<Bus *>(p)->memory[a & 0xffff];
    return 1;
}
static void account_reads(void *p, uint32_t a, uint64_t count) {
    if ((a & 0xffff) == FLAG) static_cast<Bus *>(p)->flag_reads += count;
}
static void word(Bus &b, uint32_t a, uint16_t v) { b.memory[a] = v >> 8; b.memory[a + 1] = v; }

static H8CPU *create(Bus &b, bool skip) {
    word(b, 0, 0); word(b, 2, 0x100);             // reset vector
    word(b, 0x30, 0); word(b, 0x32, 0x200);       // IRQ0 vector 12
    word(b, 0x100, 0x7a07); word(b, 0x102, 0); word(b, 0x104, 0x1000); // MOV.L #$1000,ER7
    word(b, 0x106, 0x0700);                       // LDC #0,CCR
    word(b, 0x108, 0xf801);                       // MOV.B #1,R0L
    word(b, 0x10a, 0x6aa8); word(b, 0x10c, 0); word(b, 0x10e, FLAG); // MOV.B R0L,@FLAG
    word(b, 0x110, 0x6a28); word(b, 0x112, 0); word(b, 0x114, FLAG); // MOV.B @FLAG,R0L
    word(b, 0x116, 0x46f8);                       // BNE $110
    word(b, 0x118, 0x0a0d);                       // INC.B R5L
    word(b, 0x11a, 0x40ec);                       // BRA $108
    word(b, 0x200, 0xf900);                       // MOV.B #0,R1L
    word(b, 0x202, 0x6aa9); word(b, 0x204, 0); word(b, 0x206, FLAG); // MOV.B R1L,@FLAG
    word(b, 0x208, 0x5670);                       // RTE
    H8Config cfg{};
    cfg.opaque = &b; cfg.read8 = read8; cfg.write8 = write8; cfg.adc_read = adc_read;
    if (skip) { cfg.peek8 = peek8; cfg.account_reads = account_reads; }
    b.cpu = h8_create(&cfg);
    assert(b.cpu);
    h8_write_byte(b.cpu, 0xfffff4, 0x01);         // ISCR: IRQ0 edge-triggered
    h8_write_byte(b.cpu, 0xfffff5, 0x01);         // IER: IRQ0 enabled
    h8_write_byte(b.cpu, 0xffffe9, 0x80);         // ADCR: external trigger
    return b.cpu;
}

static void same(Bus &a, Bus &b, unsigned step) {
    H8Status sa{}, sb{};
    h8_get_status(a.cpu, &sa);
    h8_get_status(b.cpu, &sb);
    bool equal = sa.pc == sb.pc && !memcmp(sa.er, sb.er, sizeof(sa.er)) && sa.ccr == sb.ccr &&
        sa.cycles == sb.cycles && sa.instructions == sb.instructions &&
        sa.sleeping == sb.sleeping && sa.halted == sb.halted &&
        h8_current_cycle(a.cpu) == h8_current_cycle(b.cpu) && a.memory == b.memory &&
        a.flag_reads == b.flag_reads;
    for (uint32_t r = 0xffffe0; equal && r <= 0xffffe9; r++)
        equal = h8_read_byte(a.cpu, r) == h8_read_byte(b.cpu, r);
    if (!equal) {
        fprintf(stderr, "step %u: pc %06x/%06x cycles %llu/%llu insns %llu/%llu reads %llu/%llu\n",
            step, sa.pc, sb.pc, (unsigned long long)sa.cycles, (unsigned long long)sb.cycles,
            (unsigned long long)sa.instructions, (unsigned long long)sb.instructions,
            (unsigned long long)a.flag_reads, (unsigned long long)b.flag_reads);
        assert(false);
    }
    assert(!sa.halted);
}

int main() {
    Bus skip, plain;
    create(skip, true);
    create(plain, false);
    uint64_t target = 0, spins = 0;
    uint32_t seed = 12345;
    for (unsigned step = 0; step < 4000; step++) {
        seed = seed * 1103515245u + 12345u;
        target += 1 + (seed >> 8) % (step % 7 == 0 ? 20000 : 600);
        // Some runs span over a second of H8 cycles, beyond one skip's limit.
        if (step % 997 == 500) target += 40000000;
        assert(h8_run_until(skip.cpu, target) == 0);
        assert(h8_run_until(plain.cpu, target) == 0);
        same(skip, plain, step);
        // Deliver the same external edges at the same deadline to both.
        if (seed & 0x10000) {
            h8_set_irq(skip.cpu, 0, 1); h8_set_irq(plain.cpu, 0, 1);
            h8_set_irq(skip.cpu, 0, 0); h8_set_irq(plain.cpu, 0, 0);
        }
        if ((seed & 0x60000) == 0x60000) {
            h8_adc_trigger(skip.cpu, 1); h8_adc_trigger(plain.cpu, 1);
            h8_adc_trigger(skip.cpu, 0); h8_adc_trigger(plain.cpu, 0);
        }
        spins = skip.memory[FLAG] ? spins + 1 : spins;
    }
    H8Status s{};
    h8_get_status(plain.cpu, &s);
    // The program must really spin and leave the spin through the IRQ.
    assert(spins > 100 && (s.er[5] & 0xff) != 0 && plain.flag_reads > 100000);
    // ...and the skip must really replace most of those reads.
    assert(skip.bus_reads * 4 < plain.bus_reads);
    printf("H8 spin skip matches plain execution: %llu instructions, %llu flag reads "
        "(%llu performed with skip), R5L=%u\n",
        (unsigned long long)s.instructions, (unsigned long long)plain.flag_reads,
        (unsigned long long)skip.bus_reads, (unsigned)(s.er[5] & 0xff));
    h8_destroy(skip.cpu);
    h8_destroy(plain.cpu);
    return 0;
}
