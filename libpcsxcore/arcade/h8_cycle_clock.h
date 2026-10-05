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
    H8CycleClock() : m_time{0,0}, m_reset_ticks(0) {}
    static Time fromTicks(uint64_t cycles);
    static uint64_t asTicks(Time time);
    static Time neverTime();
    static Time addTime(Time first, Time delta);
    void advance(uint64_t cycles);
    void resetAtCycle(uint64_t old_cycle, uint64_t new_cycle);
    uint64_t nowAsCycles(uint64_t charged = 0) const;
    uint64_t nowAsCyclesAt(Time time) const;
    Time localTime(uint64_t charged = 0) const;
    Time time() const { return m_time; }
private:
    static Time add(Time first, Time second);
    static Time subtract(Time first, Time second);
    Time m_time;
    uint64_t m_reset_ticks;
};
}
#endif
