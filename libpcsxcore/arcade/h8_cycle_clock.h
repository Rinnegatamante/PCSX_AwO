// SPDX-License-Identifier: BSD-3-Clause
// Clock conversions follow MAME attotime/device_execute_interface.
// Copyright Olivier Galibert (H8 integration), Aaron Giles (attotime).
#ifndef PCSX_ARCADE_H8_CYCLE_CLOCK_H
#define PCSX_ARCADE_H8_CYCLE_CLOCK_H
#include <cstdint>

namespace arcade {
/* H83002 local attotime advances separately from executed-cycle accounting.
 * In MAME now_as_cycles() floors scheduler attotime to ticks and subtracts
 * the reset-time tick anchor; total_cycles() instead counts charged cycles.
 * Keep the fractional phase across soft reset. All arithmetic fits 64 bits. */
class H8CycleClock {
public:
    struct Time { uint64_t seconds, attoseconds; };
    static const uint32_t frequency = 16934400;
    static const uint64_t period = UINT64_C(59051398337);
    H8CycleClock() : m_time{0,0}, m_reset_ticks(0), m_pending(0) {}
    static Time fromTicks(uint64_t cycles);
    static uint64_t asTicks(Time time);
    static Time neverTime();
    static Time addTime(Time first, Time delta);
    // Runs after every H8 instruction, so cycles are accumulated and applied
    // when time is read. While their sum stays under one second this equals
    // the per-call from_ticks() sum: each step adds cycles*period with at
    // most one carry overall, and no 64-bit division (a library call on the
    // Vita's CPU) is needed.
    void advance(uint64_t cycles) {
        if(m_pending+cycles<frequency) {m_pending+=cycles;return;}
        flush();
        if(cycles>=frequency) advanceSlow(cycles);
        else m_pending=cycles;
    }
    void resetAtCycle(uint64_t old_cycle, uint64_t new_cycle);
    uint64_t nowAsCycles(uint64_t charged = 0) const;
    uint64_t nowAsCyclesAt(Time time) const;
    Time localTime(uint64_t charged = 0) const;
    Time time() const { flush(); return m_time; }
private:
    void flush() const {
        if(!m_pending) return;
        m_time.attoseconds += m_pending*period;
        m_pending = 0;
        if(m_time.attoseconds >= UINT64_C(1000000000000000000)) {
            m_time.attoseconds -= UINT64_C(1000000000000000000);
            m_time.seconds++;
        }
    }
    void advanceSlow(uint64_t cycles);
    static Time add(Time first, Time second);
    static Time subtract(Time first, Time second);
    mutable Time m_time;
    uint64_t m_reset_ticks;
    mutable uint64_t m_pending;
};
}
#endif
