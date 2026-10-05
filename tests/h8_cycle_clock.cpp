// Check H8 attotime conversion and phase against MAME's integer formulas.
#include "../libpcsxcore/arcade/h8_cycle_clock.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using arcade::H8CycleClock;

static const uint64_t second = UINT64_C(1000000000000000000);

static bool same(H8CycleClock::Time a, H8CycleClock::Time b) {
    return a.seconds == b.seconds && a.attoseconds == b.attoseconds;
}

static void from_ticks_edges() {
    const uint64_t f = H8CycleClock::frequency;
    const uint64_t p = H8CycleClock::period;
    assert(p == second / f);
    assert(H8CycleClock::fromTicks(0).seconds == 0);
    assert(H8CycleClock::fromTicks(0).attoseconds == 0);
    assert(same(H8CycleClock::fromTicks(f - 1), {0, (f - 1) * p}));
    assert(same(H8CycleClock::fromTicks(f), {1, 0}));
    assert(same(H8CycleClock::fromTicks(f + 1), {1, p}));

    const uint64_t long_cycles = (UINT64_C(1) << 32) + 12345;
    assert(same(H8CycleClock::fromTicks(long_cycles),
                {long_cycles / f, (long_cycles % f) * p}));
}

static void as_ticks_edges() {
    const uint64_t p = H8CycleClock::period;
    assert(H8CycleClock::asTicks({0, 0}) == 0);
    // MAME floors one cycle's attosecond period, so this exact boundary is
    // still just below one tick. The next attosecond crosses it.
    assert(H8CycleClock::asTicks({0, p}) == 0);
    assert(H8CycleClock::asTicks({0, p + 1}) == 1);
    assert(H8CycleClock::asTicks({1, 0}) == H8CycleClock::frequency);
    assert(H8CycleClock::asTicks({1, p}) == H8CycleClock::frequency);
    assert(H8CycleClock::asTicks({1, p + 1}) == H8CycleClock::frequency + 1);

    const uint64_t long_cycles = (UINT64_C(1) << 32) + 12345;
    const H8CycleClock::Time long_time = H8CycleClock::fromTicks(long_cycles);
    // Check the source conversion directly. A subsecond remainder can map to
    // one fewer cycle because MAME floors both the tick period and as_ticks.
    const uint64_t expected = long_time.seconds * H8CycleClock::frequency +
        (long_time.attoseconds / UINT64_C(1000000000) * H8CycleClock::frequency +
         (long_time.attoseconds % UINT64_C(1000000000)) * H8CycleClock::frequency /
             UINT64_C(1000000000)) /
            UINT64_C(1000000000);
    assert(H8CycleClock::asTicks(long_time) == expected);
}

static void per_slice_accumulation() {
    const uint64_t f = H8CycleClock::frequency;
    const uint64_t p = H8CycleClock::period;
    H8CycleClock split, whole;
    split.advance(1);
    split.advance(f - 1);
    whole.advance(f);
    // MAME adds each slice's floor-period attotime. A single full-second
    // conversion is exactly one second, while split subsecond slices retain
    // the per-cycle truncation in local_time().
    assert(same(split.time(), {0, f * p}));
    assert(same(whole.time(), {1, 0}));
    assert(split.nowAsCycles() == f - 1);
    assert(whole.nowAsCycles() == f);

    H8CycleClock slices;
    slices.advance(f / 3);
    slices.advance(f / 3);
    slices.advance(f - 2 * (f / 3));
    assert(same(slices.time(), {0, f * p}));
    assert(slices.nowAsCycles() == f - 1);
}

static void reset_anchor_and_cycle_jumps() {
    const uint64_t f = H8CycleClock::frequency;
    H8CycleClock clock;

    // A soft reset at the same absolute cycle keeps the attotime fraction and
    // reanchors only the tick origin.
    clock.advance(1);
    clock.resetAtCycle(1, 1);
    assert(clock.nowAsCycles() == 0);
    clock.advance(f - 1);
    assert(clock.nowAsCycles() == f - 1);
    clock.resetAtCycle(f, f);
    assert(clock.nowAsCycles() == 0);
    clock.advance(1);
    assert(clock.nowAsCycles() == 1);

    // A positive epoch jump advances the source local time before anchoring.
    H8CycleClock positive;
    positive.advance(100);
    positive.resetAtCycle(100, 137);
    assert(positive.nowAsCycles() == 0);
    positive.advance(1);
    assert(positive.nowAsCycles() == 1);

    // Reset can discard outstanding execution overshoot. Rewind the local
    // timeline by the same amount, then establish a fresh zero tick anchor.
    H8CycleClock rewind;
    rewind.advance(300);
    rewind.resetAtCycle(300, 292);
    assert(rewind.nowAsCycles() == 0);
    rewind.advance(1);
    assert(rewind.nowAsCycles() == 1);
}

static void charged_cycles_use_attotime_phase() {
    H8CycleClock clock;
    clock.advance(1);
    clock.resetAtCycle(1, 1);
    assert(clock.nowAsCycles() == 0);
    // TX/local time counts the charged cycle, but the scheduler-domain RX
    // conversion observes the retained fractional attotime phase exactly.
    assert(clock.nowAsCycles(1) == 1);
    clock.resetAtCycle(1, 1);
    assert(clock.nowAsCycles() == 0);
    assert(clock.nowAsCycles(1) == 1);
}

int main() {
    from_ticks_edges();
    as_ticks_edges();
    per_slice_accumulation();
    reset_anchor_and_cycle_jumps();
    charged_cycles_use_attotime_phase();
    std::puts("H8 cycle-clock tests passed");
}
