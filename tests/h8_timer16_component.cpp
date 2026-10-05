// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/h8_timer16.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using arcade::H8Timer16;

struct Irqs {
    std::vector<unsigned> vectors;
    unsigned reschedules = 0;
};

static void irq(void *opaque, unsigned vector)
{
    static_cast<Irqs *>(opaque)->vectors.push_back(vector);
}

static void reschedule(void *opaque)
{
    ++static_cast<Irqs *>(opaque)->reschedules;
}

static uint16_t readCounter(H8Timer16 &timer, unsigned channel, uint64_t cycle)
{
    static const uint16_t base[5] = { 0x64, 0x6e, 0x78, 0x82, 0x92 };
    return timer.readWord(base[channel] + 4, cycle);
}

static uint8_t readStatus(H8Timer16 &timer, unsigned channel, uint64_t cycle)
{
    static const uint16_t base[5] = { 0x64, 0x6e, 0x78, 0x82, 0x92 };
    return timer.readByte(base[channel] + 3, cycle);
}

static void configure(H8Timer16 &timer, unsigned channel, uint8_t tcr,
                      uint64_t cycle = 0)
{
    static const uint16_t base[5] = { 0x64, 0x6e, 0x78, 0x82, 0x92 };
    timer.writeByte(base[channel] + 0, tcr, cycle);
}

static void internal_dividers_and_phase()
{
    H8Timer16 timer;
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x00);
    assert(readCounter(timer, 0, 17) == 17);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x01);
    assert(readCounter(timer, 0, 1) == 0);
    assert(readCounter(timer, 0, 2) == 1);
    assert(readCounter(timer, 0, 4) == 2);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x02);
    assert(readCounter(timer, 0, 3) == 0);
    assert(readCounter(timer, 0, 4) == 1);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x0a); // /4, 180-degree phase: first tick at cycle 2.
    assert(readCounter(timer, 0, 1) == 0);
    assert(readCounter(timer, 0, 2) == 1);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x12); // /4 source, both edges: effective /2.
    assert(readCounter(timer, 0, 1) == 0);
    assert(readCounter(timer, 0, 2) == 1);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x03);
    assert(readCounter(timer, 0, 7) == 0);
    assert(readCounter(timer, 0, 8) == 1);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x0b); // /8, 180-degree phase: first tick at cycle 4.
    assert(readCounter(timer, 0, 3) == 0);
    assert(readCounter(timer, 0, 4) == 1);

    timer.reset();
    timer.writeByte(0x60, 1, 0);
    configure(timer, 0, 0x13); // /8 source, both edges: effective /4.
    assert(readCounter(timer, 0, 3) == 0);
    assert(readCounter(timer, 0, 4) == 1);
}

static void compare_clear_and_interrupt_deadline()
{
    H8Timer16 timer;
    Irqs raised;
    timer.setCallbacks(&raised, irq, reschedule);
    timer.writeWord(0x6a, 2, 0xffff, 0); // GRA=2, match at TCNT=3.
    timer.writeByte(0x66, 1, 0);        // TGA interrupt enable.
    configure(timer, 0, 0x00);
    timer.writeByte(0x60, 1, 0);
    assert(timer.nextDeadline() == 3);
    const unsigned scheduled_count = raised.reschedules;
    timer.writeByte(0x60, 0, 0); // MAME's inactive recalc path does not reschedule CPU.
    assert(raised.reschedules == scheduled_count);
    timer.writeByte(0x60, 1, 0);
    assert(raised.reschedules > scheduled_count);
    timer.update(2);
    assert(raised.vectors.empty());
    timer.update(3);
    assert(raised.vectors.size() == 1 && raised.vectors[0] == 24);
    assert(readStatus(timer, 0, 3) == 0xf9);
    assert(readCounter(timer, 0, 3) == 3);
    assert(raised.reschedules != 0);

    // A timestamped register write first drains the already scheduled event,
    // then changes the compare register at that cycle.
    timer.reset();
    raised.vectors.clear();
    timer.writeWord(0x6a, 2, 0xffff, 0);
    timer.writeByte(0x66, 1, 0);
    configure(timer, 0, 0);
    timer.writeByte(0x60, 1, 0);
    timer.writeWord(0x6a, 10, 0xffff, 3);
    assert(raised.vectors.size() == 1 && raised.vectors[0] == 24);
    assert(readCounter(timer, 0, 3) == 3);

    // A flag is set even when its IRQ is disabled, and TSR is W0C.
    timer.reset();
    timer.writeWord(0x6a, 2, 0xffff, 0);
    configure(timer, 0, 0x00);
    timer.writeByte(0x60, 1, 0);
    timer.update(3);
    assert(timer.nextDeadline() == UINT64_MAX);
    assert(readStatus(timer, 0, 3) == 0xf9);
    timer.writeByte(0x67, 0xf8, 3);
    assert(readStatus(timer, 0, 3) == 0xf8);

    // Clear on A uses GRA+1 as the period and does not count a 16-bit overflow.
    timer.reset();
    timer.writeWord(0x6a, 2, 0xffff, 0);
    configure(timer, 0, 0x20);
    timer.writeByte(0x60, 1, 0);
    assert(readCounter(timer, 0, 1) == 1);
    assert(readCounter(timer, 0, 2) == 2);
    assert(readCounter(timer, 0, 3) == 0);
    assert(readCounter(timer, 0, 6) == 0);
    assert((readStatus(timer, 0, 6) & 0x05) == 0x01);
}

static void overflow_pause_and_word_lanes()
{
    H8Timer16 timer;
    Irqs raised;
    timer.setCallbacks(&raised, irq);
    timer.writeByte(0x66, 4, 0); // Overflow IRQ.
    configure(timer, 0, 0);
    timer.writeByte(0x60, 1, 0);
    timer.writeWord(0x68, 0xfffe, 0xffff, 0);
    assert(timer.nextDeadline() == 2);
    timer.update(1);
    assert(readCounter(timer, 0, 1) == 0xffff);
    timer.update(2);
    assert(readCounter(timer, 0, 2) == 0);
    // Reset GRA/GRB are FFFF, so their wrapped compare flags also set at zero.
    assert(readStatus(timer, 0, 2) == 0xff);
    assert(raised.vectors.size() == 1 && raised.vectors[0] == 26);

    timer.reset();
    configure(timer, 0, 0);
    timer.writeByte(0x60, 1, 0);
    assert(readCounter(timer, 0, 3) == 3);
    timer.writeByte(0x60, 0, 3);
    assert(readCounter(timer, 0, 10) == 3);
    timer.writeByte(0x60, 1, 10);
    assert(readCounter(timer, 0, 12) == 5);

    // Atomic word lane masks follow the H8 big-endian convention.
    timer.writeWord(0x68, 0xabcd, 0xffff, 12);
    timer.writeWord(0x68, 0x1200, 0xff00, 12);
    timer.writeWord(0x68, 0x0034, 0x00ff, 12);
    assert(timer.readWord(0x68, 12) == 0x1234);

    // H8H TIER reserved bits become ones after its first write; reset is zero.
    timer.reset();
    assert(timer.readByte(0x66, 0) == 0);
    timer.writeByte(0x66, 0x05, 0);
    assert(timer.readByte(0x66, 0) == 0xfd);
}

static void register_map_and_buffers()
{
    H8Timer16 timer;
    assert(timer.readByte(0x60, 0) == 0xe0);
    assert(timer.readByte(0x61, 0) == 0);
    assert(timer.readByte(0x64, 0) == 0);
    assert(timer.readWord(0x68, 0) == 0);
    assert(timer.readWord(0x6a, 0) == 0xffff);
    assert(timer.readByte(0x67, 0) == 0xf8);
    timer.writeByte(0x61, 0xff, 0);
    timer.writeByte(0x65, 0xff, 0);
    timer.writeByte(0x62, 0xff, 0);
    timer.writeByte(0x63, 0xff, 0);
    timer.writeByte(0x90, 0xff, 0);
    timer.writeByte(0x91, 0xff, 0);
    assert(timer.readByte(0x61, 0) == 0);
    assert(timer.readByte(0x65, 0) == 0);
    assert(timer.readByte(0x62, 0) == 0);
    assert(timer.readByte(0x63, 0) == 0);
    assert(timer.readByte(0x90, 0) == 0);
    assert(timer.readByte(0x91, 0) == 0);

    timer.writeWord(0x8c, 0x1234, 0xffff, 0); // Channel 3 TBR A.
    assert(timer.readWord(0x8c, 0) == 0x1234);
    assert(timer.readWord(0x88, 0) == 0xffff); // TBR does not alter TGR A.
}

int main()
{
    internal_dividers_and_phase();
    compare_clear_and_interrupt_deadline();
    overflow_pause_and_word_lanes();
    register_map_and_buffers();
    std::puts("H8/3002 timer16 component tests passed");
}
