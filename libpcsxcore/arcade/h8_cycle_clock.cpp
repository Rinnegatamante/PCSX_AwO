// SPDX-License-Identifier: BSD-3-Clause
// MAME attotime.h, diexec.cpp, schedule.cpp at
// deb2f54fe7300e7a192690318beb860159bfd52a. See h8_LICENSE.txt.
#include "h8_cycle_clock.h"

namespace arcade {
static const uint64_t second = UINT64_C(1000000000000000000);
static const uint64_t sqrt_second = UINT64_C(1000000000);

H8CycleClock::Time H8CycleClock::fromTicks(uint64_t cycles) {
    return Time{cycles / frequency, (cycles % frequency) * period};
}
H8CycleClock::Time H8CycleClock::neverTime() {
    return Time{UINT64_C(1000000000),0};
}
uint64_t H8CycleClock::asTicks(Time time) {
    // attotime::operator*(u32) splits attoseconds into billion-sized limbs.
    uint64_t product = (time.attoseconds % sqrt_second) * frequency;
    product = product / sqrt_second + (time.attoseconds / sqrt_second) * frequency;
    return time.seconds * frequency + product / sqrt_second;
}
H8CycleClock::Time H8CycleClock::add(Time first, Time delta) {
    first.seconds += delta.seconds;
    first.attoseconds += delta.attoseconds;
    if(first.attoseconds >= second) {
        first.attoseconds -= second;
        first.seconds++;
    }
    return first;
}
H8CycleClock::Time H8CycleClock::subtract(Time first, Time delta) {
    if(first.seconds < delta.seconds ||
       (first.seconds == delta.seconds && first.attoseconds < delta.attoseconds))
        return Time{0,0};
    if(first.attoseconds < delta.attoseconds) {
        first.attoseconds += second;
        first.seconds--;
    }
    first.seconds -= delta.seconds;
    first.attoseconds -= delta.attoseconds;
    return first;
}
H8CycleClock::Time H8CycleClock::addTime(Time first, Time delta) {
    return add(first,delta);
}
void H8CycleClock::advance(uint64_t cycles) {
    // Match local_time() / scheduler accounting: per-slice from_ticks, then
    // accumulate attotime, rather than round the aggregate cycle count.
    m_time = add(m_time,fromTicks(cycles));
}
void H8CycleClock::resetAtCycle(uint64_t old_cycle,uint64_t new_cycle) {
    // The public reset API may remove outstanding overshoot, or supply an
    // artificial future epoch. Re-anchor its local time by that same delta.
    if(new_cycle >= old_cycle)m_time = add(m_time,fromTicks(new_cycle-old_cycle));
    else m_time = subtract(m_time,fromTicks(old_cycle-new_cycle));
    m_reset_ticks = asTicks(m_time);
}
uint64_t H8CycleClock::nowAsCycles(uint64_t charged) const {
    return nowAsCyclesAt(localTime(charged));
}
uint64_t H8CycleClock::nowAsCyclesAt(Time time) const {
    return asTicks(time) - m_reset_ticks;
}
H8CycleClock::Time H8CycleClock::localTime(uint64_t charged) const {
    return add(m_time,fromTicks(charged));
}
}
