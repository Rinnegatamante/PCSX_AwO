// SPDX-License-Identifier: BSD-3-Clause
// Differential coverage for the H8 IRQ candidate-scan optimization.
// Include the implementation so this test can exercise its private state
// without adding test hooks to the public emulator API.
#include "../libpcsxcore/arcade/h8.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>

static int reference_irq_filter(H8CPU &cpu) {
    cpu.io[0xf6] |= cpu.irq_lines & u8(~cpu.io[0xf4]);
    u64 candidates = cpu.pending;
    for (unsigned i = 0; i < 6; ++i)
        if ((cpu.io[0xf5] & cpu.io[0xf6]) & (1u << i))
            candidates |= u64(1) << (12 + i);

    int filter = 0;
    if (cpu.io[0xf2] & 8)
        filter = (cpu.m_CCR & H8CPU::F_I) ? 2 : 0;
    else if ((cpu.m_CCR & (H8CPU::F_I | H8CPU::F_UI)) ==
             (H8CPU::F_I | H8CPU::F_UI))
        filter = 2;
    else if (cpu.m_CCR & H8CPU::F_I)
        filter = 1;

    int chosen = 0;
    int priority = -1;
    unsigned icr = cpu.io[0xf8] | (unsigned(cpu.io[0xf9]) << 8);
    for (unsigned i = 7; i < 64; ++i) {
        if (!(candidates & (u64(1) << i)))
            continue;
        int p = i == 7 ? 2 :
            (vector_slot[i] < 0 ? 0 : (icr >> (vector_slot[i] ^ 7)) & 1);
        if (p >= filter && p > priority) {
            chosen = static_cast<int>(i);
            priority = p;
        }
    }
    cpu.m_irq_vector = chosen;
    return chosen;
}

struct IRQInputs {
    u64 pending;
    u8 ccr, syscr, icr_lo, icr_hi;
    u8 irq_lines, irq_mask, irq_enable, irq_status;
};

static void seed(H8CPU &cpu, const IRQInputs &in) {
    cpu.pending = in.pending;
    cpu.m_CCR = in.ccr;
    cpu.io[0xf2] = in.syscr;
    cpu.io[0xf4] = in.irq_mask;
    cpu.io[0xf5] = in.irq_enable;
    cpu.io[0xf6] = in.irq_status;
    cpu.io[0xf8] = in.icr_lo;
    cpu.io[0xf9] = in.icr_hi;
    cpu.irq_lines = in.irq_lines;
    cpu.m_irq_vector = -1;
}

static void check(const IRQInputs &in) {
    H8Config config{};
    H8CPU actual(config);
    H8CPU expected(config);
    seed(actual, in);
    seed(expected, in);

    const int reference = reference_irq_filter(expected);
    actual.update_irq_filter();
    assert(actual.m_irq_vector == reference);
    assert(actual.io[0xf6] == expected.io[0xf6]);
}

static uint64_t next_random(uint64_t &state) {
    // xorshift64*: deterministic across standard-library implementations.
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

static IRQInputs input(u64 pending, u8 ccr = 0, u8 syscr = 0,
                       u8 icr_lo = 0, u8 icr_hi = 0,
                       u8 irq_lines = 0, u8 irq_mask = 0,
                       u8 irq_enable = 0, u8 irq_status = 0xc0) {
    IRQInputs result = {pending, ccr, syscr, icr_lo, icr_hi,
                        irq_lines, irq_mask, irq_enable, irq_status};
    return result;
}

int main() {
    // Empty, ignored low vectors, NMI acceptance, ordinary priority filtering,
    // and the external request register's six implemented input bits.
    check(input(0));
    check(input(UINT64_C(0x7f)));
    check(input(UINT64_C(1) << 7, H8CPU::F_I | H8CPU::F_UI));
    check(input(UINT64_C(1) << 63, H8CPU::F_I, 0, 0, 0x02));
    check(input(UINT64_C(1) << 63, H8CPU::F_I, 0, 0, 0x00));
    check(input(0, 0, 0, 0, 0, 0, 0, 0xc0)); // enable bits 6/7 are ignored
    check(input(0, 0, 0, 0, 0, 0, 0, 0xff));
    check(input(0, 0, 0, 0, 0, 0xc0, 0, 0xff)); // request bits 6/7 are ignored
    check(input(0, 0, 0, 0, 0, 0x03, 0, 0x03)); // requests map to vectors 12/13
    check(input(0, 0, 0, 0, 0, 0, 0, 0x3f, 0x3f)); // existing latches map to vector 12 first
    check(input(UINT64_C(1) << 63 | UINT64_C(1) << 12,
                0, 0, 0, 0x02)); // priority 1 beats priority 0
    check(input(UINT64_C(1) << 63 | UINT64_C(1) << 62,
                0, 0, 0, 0x02)); // equal priority keeps lower vector 62

    // Every one-hot vector, and every two-hot pair, including bits that the
    // current device does not generate. Those still follow the original scan.
    for (unsigned i = 0; i < 64; ++i) {
        const u64 one = u64(1) << i;
        check(input(one));
        check(input(one, H8CPU::F_I));
        check(input(one, H8CPU::F_I | H8CPU::F_UI));
        check(input(one, H8CPU::F_I, 8));
        for (unsigned j = i + 1; j < 64; ++j)
            check(input(one | (u64(1) << j), 0, 0,
                        static_cast<u8>(i * 13), static_cast<u8>(j * 7)));
    }

    // Exercise the level-latch side effect and varied priority/mask combinations.
    for (unsigned lines = 0; lines < 64; ++lines) {
        for (unsigned mask = 0; mask < 64; mask += 7) {
            for (unsigned enable = 0; enable < 64; enable += 11) {
                check(input(UINT64_C(1) << ((lines + mask + enable) % 64),
                            static_cast<u8>((lines * 17) & 0xff),
                            static_cast<u8>((mask & 1) ? 8 : 0),
                            static_cast<u8>(enable * 3),
                            static_cast<u8>(lines * 5),
                            static_cast<u8>(lines), static_cast<u8>(mask),
                            static_cast<u8>(enable)));
            }
        }
    }

    uint64_t rng = UINT64_C(0x4d595df4d0f33173);
    for (unsigned n = 0; n < 100000; ++n) {
        const u64 pending = next_random(rng);
        const u64 random = next_random(rng);
        check(input(pending, static_cast<u8>(random),
                    static_cast<u8>(random >> 8),
                    static_cast<u8>(random >> 16),
                    static_cast<u8>(random >> 24),
                    static_cast<u8>(random >> 32),
                    static_cast<u8>(random >> 40),
                    static_cast<u8>(random >> 48),
                    static_cast<u8>(random >> 56)));
    }

    std::puts("H8 IRQ candidate filter matches the original scan");
    return 0;
}
