// SPDX-License-Identifier: BSD-3-Clause
// Adapted from MAME src/devices/cpu/h8/h8_timer16.cpp.
// Copyright (C) Olivier Galibert

#include "h8_timer16.h"

#include <algorithm>
#include <cstring>

namespace arcade {
namespace {

const uint16_t kChannelBase[5] = { 0x64, 0x6e, 0x78, 0x82, 0x92 };
const unsigned kIrqBase[5] = { 24, 28, 32, 36, 40 };
const uint64_t kNoDeadline = UINT64_MAX;
enum { kClearNone = -1, kClearExternal = -2 };
enum { kIrqA = 0x01, kIrqB = 0x02, kIrqV = 0x10 };

} // anonymous namespace

H8Timer16::H8Timer16() noexcept : m_opaque(nullptr), m_irq(nullptr),
    m_reschedule(nullptr), m_tstr(0)
{
    reset();
}

void H8Timer16::setCallbacks(void *opaque, IrqCallback irq,
                             RescheduleCallback reschedule) noexcept
{
    m_opaque = opaque;
    m_irq = irq;
    m_reschedule = reschedule;
}

void H8Timer16::reset(uint64_t cycle) noexcept
{
    m_tstr = 0xe0;
    for (unsigned i = 0; i < 5; ++i) {
        Channel &ch = m_channel[i];
        std::memset(ch.tgr, 0xff, sizeof(ch.tgr));
        ch.tcnt = 0;
        ch.last_clock_update = cycle;
        ch.event_time = 0;
        ch.phase = 0;
        ch.counter_cycle = 0x10000;
        ch.tcr = 0;
        ch.tier = 0; // MAME reset uses 0x40 & the H8H tier mask (zero).
        ch.ier = 0;
        ch.isr = 0;
        ch.clock_divider = 0;
        ch.tgr_clearing = kClearNone;
        ch.active = false;
    }
    m_next_deadline = kNoDeadline;
}

int H8Timer16::channelForOffset(uint16_t offset) const noexcept
{
    for (unsigned i = 0; i < 5; ++i) {
        if (offset >= kChannelBase[i] && offset < kChannelBase[i] + 0x0a)
            return static_cast<int>(i);
        if ((i == 3 || i == 4) && offset >= kChannelBase[i] + 0x0a &&
            offset < kChannelBase[i] + 0x0e)
            return static_cast<int>(i);
    }
    return -1;
}

uint8_t H8Timer16::readByte(uint16_t offset, uint64_t access_cycle)
{
    return readByteAt(offset, access_cycle);
}

uint8_t H8Timer16::readByteAt(uint16_t offset, uint64_t access_cycle)
{
    offset &= 0xff;
    if (offset == 0x60)
        return m_tstr;
    if (offset == 0x61 || offset == 0x62 || offset == 0x63 ||
        offset == 0x90 || offset == 0x91)
        return 0;

    const int index = channelForOffset(offset);
    if (index < 0)
        return 0xff;
    Channel &ch = m_channel[index];
    const unsigned rel = offset - kChannelBase[index];
    switch (rel) {
    case 0: return ch.tcr;
    case 1: return 0; // TIOR is not implemented by MAME's H8H timer channel.
    case 2: return ch.tier;
    case 3:
        updateCounter(static_cast<unsigned>(index), access_cycle);
        return static_cast<uint8_t>(0xf8 | ((ch.isr & kIrqV) ? 4 : 0) |
                                    ((ch.isr & kIrqB) ? 2 : 0) |
                                    ((ch.isr & kIrqA) ? 1 : 0));
    case 4:
        updateCounter(static_cast<unsigned>(index), access_cycle);
        return static_cast<uint8_t>(ch.tcnt >> 8);
    case 5:
        updateCounter(static_cast<unsigned>(index), access_cycle);
        return static_cast<uint8_t>(ch.tcnt);
    case 6: case 7: case 8: case 9: {
        const uint16_t value = ch.tgr[(rel - 6) >> 1];
        return static_cast<uint8_t>((rel & 1) ? value : value >> 8);
    }
    case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        if (index == 3 || index == 4) {
            const uint16_t value = ch.tgr[2 + ((rel - 0x0a) >> 1)];
            return static_cast<uint8_t>((rel & 1) ? value : value >> 8);
        }
        break;
    default: break;
    }
    return 0xff;
}

void H8Timer16::writeByte(uint16_t offset, uint8_t value, uint64_t access_cycle)
{
    writeByteAt(offset, value, access_cycle);
}

void H8Timer16::writeByteAt(uint16_t offset, uint8_t value, uint64_t access_cycle)
{
    offset &= 0xff;
    if (offset == 0x60) {
        m_tstr = value;
        for (unsigned i = 0; i < 5; ++i)
            setEnable(i, (value & (1u << i)) != 0, access_cycle);
        return;
    }
    if (offset == 0x61 || offset == 0x62 || offset == 0x63 ||
        offset == 0x90 || offset == 0x91)
        return;

    const int index = channelForOffset(offset);
    if (index < 0)
        return;
    Channel &ch = m_channel[index];
    const unsigned rel = offset - kChannelBase[index];
    switch (rel) {
    case 0: setTcr(index, value, access_cycle); break;
    case 1: break; // TIOR writes are ignored.
    case 2: setTier(index, value, access_cycle); break;
    case 3: setTsr(index, value, access_cycle); break;
    case 4:
    case 5:
        updateCounter(index, access_cycle);
        if (rel == 4)
            ch.tcnt = static_cast<uint16_t>((ch.tcnt & 0x00ff) | (uint16_t(value) << 8));
        else
            ch.tcnt = static_cast<uint16_t>((ch.tcnt & 0xff00) | value);
        recalcEvent(index, access_cycle, true);
        break;
    case 6: case 7: case 8: case 9: {
        updateCounter(index, access_cycle);
        uint16_t &target = ch.tgr[(rel - 6) >> 1];
        if (rel & 1)
            target = static_cast<uint16_t>((target & 0xff00) | value);
        else
            target = static_cast<uint16_t>((target & 0x00ff) | (uint16_t(value) << 8));
        recalcEvent(index, access_cycle, true);
        break;
    }
    case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        if (index == 3 || index == 4) {
            uint16_t &target = ch.tgr[2 + ((rel - 0x0a) >> 1)];
            if (rel & 1)
                target = static_cast<uint16_t>((target & 0xff00) | value);
            else
                target = static_cast<uint16_t>((target & 0x00ff) | (uint16_t(value) << 8));
        }
        break;
    default: break;
    }
}

uint16_t H8Timer16::readWord(uint16_t offset, uint64_t access_cycle)
{
    offset &= 0xfe;
    if (offset == 0x60)
        return static_cast<uint16_t>((uint16_t(readByteAt(offset, access_cycle)) << 8) |
                                     readByteAt(offset + 1, access_cycle));
    const int index = channelForOffset(offset);
    if (index < 0)
        return static_cast<uint16_t>((uint16_t(readByteAt(offset, access_cycle)) << 8) |
                                     readByteAt(offset + 1, access_cycle));
    Channel &ch = m_channel[index];
    const unsigned rel = offset - kChannelBase[index];
    if (rel == 4) {
        updateCounter(static_cast<unsigned>(index), access_cycle);
        return ch.tcnt;
    }
    if (rel == 6 || rel == 8)
        return ch.tgr[(rel - 6) >> 1];
    if ((rel == 0x0a || rel == 0x0c) && (index == 3 || index == 4))
        return ch.tgr[2 + ((rel - 0x0a) >> 1)];
    return static_cast<uint16_t>((uint16_t(readByteAt(offset, access_cycle)) << 8) |
                                 readByteAt(offset + 1, access_cycle));
}

void H8Timer16::writeWord(uint16_t offset, uint16_t value, uint16_t lane_mask,
                          uint64_t access_cycle)
{
    offset &= 0xfe;
    const int index = channelForOffset(offset);
    if (index >= 0) {
        Channel &ch = m_channel[index];
        const unsigned rel = offset - kChannelBase[index];
        if (rel == 4) {
            updateCounter(static_cast<unsigned>(index), access_cycle);
            ch.tcnt = static_cast<uint16_t>((ch.tcnt & ~lane_mask) | (value & lane_mask));
            recalcEvent(static_cast<unsigned>(index), access_cycle, true);
            return;
        }
        if (rel == 6 || rel == 8) {
            updateCounter(static_cast<unsigned>(index), access_cycle);
            uint16_t &target = ch.tgr[(rel - 6) >> 1];
            target = static_cast<uint16_t>((target & ~lane_mask) | (value & lane_mask));
            recalcEvent(static_cast<unsigned>(index), access_cycle, true);
            return;
        }
        if ((rel == 0x0a || rel == 0x0c) && (index == 3 || index == 4)) {
            uint16_t &target = ch.tgr[2 + ((rel - 0x0a) >> 1)];
            target = static_cast<uint16_t>((target & ~lane_mask) | (value & lane_mask));
            return;
        }
    }

    if (offset == 0x60) {
        // TSTR is byte-wide in the H8 map; a word write's high lane addresses it.
        if (lane_mask & 0xff00)
            writeByteAt(offset, static_cast<uint8_t>(value >> 8), access_cycle);
        return;
    }
    if (lane_mask & 0xff00)
        writeByteAt(offset, static_cast<uint8_t>(value >> 8), access_cycle);
    if (lane_mask & 0x00ff)
        writeByteAt(offset + 1, static_cast<uint8_t>(value), access_cycle);
}

void H8Timer16::setTcr(unsigned index, uint8_t value, uint64_t cycle)
{
    Channel &ch = m_channel[index];
    updateCounter(index, cycle);
    ch.tcr = value;
    switch (value & 0x60) {
    case 0x00: ch.tgr_clearing = kClearNone; break;
    case 0x20: case 0x40: ch.tgr_clearing = (value & 0x20) ? 0 : 1; break;
    case 0x60: ch.tgr_clearing = kClearExternal; break;
    }

    const int count_type = value & 7;
    if (count_type < 4) {
        ch.clock_divider = count_type;
        if (count_type <= 1)
            ch.phase = 0;
        else {
            switch (value & 0x18) {
            case 0x00: ch.phase = 0; break;
            case 0x08: ch.phase = 1u << (ch.clock_divider - 1); break;
            case 0x10: case 0x18: ch.phase = 0; --ch.clock_divider; break;
            }
        }
    } else {
        ch.clock_divider = -1; // H8/3002 external input clock; board pins are unwired.
        ch.phase = 0;
    }
    recalcEvent(index, cycle, true);
}

void H8Timer16::setTier(unsigned index, uint8_t value, uint64_t cycle)
{
    Channel &ch = m_channel[index];
    updateCounter(index, cycle);
    ch.tier = static_cast<uint8_t>(value | 0xf8);
    ch.ier = static_cast<uint8_t>((ch.tier & 1 ? kIrqA : 0) |
                                  (ch.tier & 2 ? kIrqB : 0) |
                                  (ch.tier & 4 ? kIrqV : 0));
    recalcEvent(index, cycle, true);
}

void H8Timer16::setTsr(unsigned index, uint8_t value, uint64_t cycle)
{
    Channel &ch = m_channel[index];
    updateCounter(index, cycle);
    if (!(value & 1)) ch.isr &= static_cast<uint8_t>(~kIrqA);
    if (!(value & 2)) ch.isr &= static_cast<uint8_t>(~kIrqB);
    if (!(value & 4)) ch.isr &= static_cast<uint8_t>(~kIrqV);
    recalcEvent(index, cycle, true);
}

void H8Timer16::setEnable(unsigned index, bool enable, uint64_t cycle)
{
    Channel &ch = m_channel[index];
    updateCounter(index, cycle);
    ch.active = enable;
    recalcEvent(index, cycle, true);
}

void H8Timer16::raiseIrq(unsigned index, unsigned flag)
{
    if (!m_irq)
        return;
    unsigned vector = kIrqBase[index];
    if (flag == kIrqB) ++vector;
    else if (flag == kIrqV) vector += 2;
    m_irq(m_opaque, vector);
}

void H8Timer16::updateCounter(unsigned index, uint64_t cycle)
{
    Channel &ch = m_channel[index];
    if (ch.clock_divider < 0)
        return;
    if (!ch.active) {
        ch.last_clock_update = cycle;
        return;
    }

    const uint64_t base_time = ch.last_clock_update;
    ch.last_clock_update = cycle;
    uint64_t base_tick = base_time;
    uint64_t new_tick = cycle;
    if (ch.clock_divider) {
        base_tick = (base_tick + ch.phase) >> ch.clock_divider;
        new_tick = (new_tick + ch.phase) >> ch.clock_divider;
    }
    if (new_tick == base_tick)
        return;

    const uint16_t prev = ch.tcnt;
    const uint64_t delta = new_tick - base_tick;
    const uint64_t total = uint64_t(ch.tcnt) + delta;
    if (prev >= ch.counter_cycle) {
        if (total >= 0x10000)
            ch.tcnt = static_cast<uint16_t>((total - 0x10000) % ch.counter_cycle);
        else
            ch.tcnt = static_cast<uint16_t>(total);
    } else {
        ch.tcnt = static_cast<uint16_t>(total % ch.counter_cycle);
    }

    for (unsigned i = 0; i < 2; ++i) {
        // Keep MAME's u16 conversion: GRA/GRB=FFFF matches at counter zero.
        const uint16_t compare = static_cast<uint16_t>(ch.tgr[i] + 1);
        bool match = ch.tcnt == compare || (total == compare && total == ch.counter_cycle);
        if (!match) {
            if (prev >= ch.counter_cycle) {
                match = (compare > prev && total >= compare) ||
                        (compare <= ch.counter_cycle && ch.tcnt < ch.counter_cycle &&
                         (delta - (0x10000 - prev)) >= compare);
            } else if (compare <= ch.counter_cycle) {
                match = delta >= ch.counter_cycle || (prev < compare && total >= compare) ||
                        (ch.tcnt <= prev && ch.tcnt >= compare);
            }
        }
        if (match) {
            ch.isr |= static_cast<uint8_t>(1u << i);
            if (ch.ier & (1u << i))
                raiseIrq(index, 1u << i);
        }
    }

    if (total >= 0x10000 && (ch.counter_cycle == 0x10000 || prev >= ch.counter_cycle)) {
        ch.isr |= kIrqV;
        if (ch.ier & kIrqV)
            raiseIrq(index, kIrqV);
    }
}

void H8Timer16::recalcEvent(unsigned index, uint64_t cycle, bool request_reschedule)
{
    Channel &ch = m_channel[index];
    const uint64_t old_event_time = ch.event_time;
    if (!ch.active) {
        setEventTime(ch, 0);
        return;
    }
    if (ch.clock_divider < 0) {
        setEventTime(ch, 0);
        if (old_event_time && request_reschedule && m_reschedule)
            m_reschedule(m_opaque);
        return;
    }

    if (ch.tgr_clearing >= 0)
        ch.counter_cycle = uint32_t(ch.tgr[ch.tgr_clearing]) + 1;
    else
        ch.counter_cycle = 0x10000;

    uint32_t event_delay = 0xffffffff;
    if ((ch.ier & kIrqV) &&
        (ch.counter_cycle == 0x10000 || ch.tcnt >= ch.counter_cycle))
        event_delay = 0x10000 - ch.tcnt;

    for (unsigned i = 0; i < 2; ++i) {
        if (!(ch.ier & (1u << i)))
            continue;
        const uint16_t compare = static_cast<uint16_t>(ch.tgr[i] + 1);
        uint32_t delay = 0xffffffff;
        if (compare > ch.tcnt) {
            if (ch.tcnt >= ch.counter_cycle || compare <= ch.counter_cycle)
                delay = compare - ch.tcnt;
        } else if (compare <= ch.counter_cycle) {
            if (ch.tcnt < ch.counter_cycle)
                delay = (ch.counter_cycle - ch.tcnt) + compare;
            else
                delay = (0x10000 - ch.tcnt) + compare;
        }
        if (event_delay > delay)
            event_delay = delay;
    }

    if (event_delay == 0xffffffff) {
        setEventTime(ch, 0);
        if (old_event_time && request_reschedule && m_reschedule)
            m_reschedule(m_opaque);
        return;
    }
    const uint64_t divisor = uint64_t(1) << ch.clock_divider;
    setEventTime(ch, ((((cycle + divisor - ch.phase) >> ch.clock_divider) +
                      event_delay - 1) << ch.clock_divider) + ch.phase);
    if (old_event_time != ch.event_time && request_reschedule && m_reschedule)
        m_reschedule(m_opaque);
}

void H8Timer16::update(uint64_t cycle)
{
    for (unsigned index = 0; index < 5; ++index) {
        Channel &ch = m_channel[index];
        if (cycle < ch.last_clock_update)
            continue;
        while (ch.event_time && cycle >= ch.event_time) {
            const uint64_t event_cycle = ch.event_time;
            updateCounter(index, event_cycle);
            recalcEvent(index, event_cycle, false);
        }
    }
}

void H8Timer16::setEventTime(Channel &channel, uint64_t when) noexcept
{
    channel.event_time = when;
    m_next_deadline = kNoDeadline;
    for (unsigned i = 0; i < 5; ++i) {
        if (m_channel[i].event_time && m_channel[i].event_time < m_next_deadline)
            m_next_deadline = m_channel[i].event_time;
    }
}

uint64_t H8Timer16::nextDeadline() const noexcept
{
    return m_next_deadline;
}

} // namespace arcade
