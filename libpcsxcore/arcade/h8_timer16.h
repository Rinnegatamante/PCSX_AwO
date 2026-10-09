/* SPDX-License-Identifier: BSD-3-Clause
 * H8/3002 16-bit timer component, adapted from MAME's h8_timer16_device.
 * Copyright (C) Olivier Galibert
 */
#ifndef PCSX_ARCADE_H8_TIMER16_H
#define PCSX_ARCADE_H8_TIMER16_H

#include <cstdint>

namespace arcade {

class H8Timer16 {
public:
    typedef void (*IrqCallback)(void *opaque, unsigned vector);
    typedef void (*RescheduleCallback)(void *opaque);

    H8Timer16() noexcept;
    void setCallbacks(void *opaque, IrqCallback irq,
                      RescheduleCallback reschedule = nullptr) noexcept;
    void reset(uint64_t cycle = 0) noexcept;

    /* Register offsets are the low byte of the H8 internal address, 0x60-0x9f.
     * Word accesses are aligned/even. lane_mask follows H8 big-endian bus bits:
     * 0xff00 selects the high byte, 0x00ff selects the low byte. */
    uint8_t readByte(uint16_t offset, uint64_t access_cycle);
    void writeByte(uint16_t offset, uint8_t value, uint64_t access_cycle);
    uint16_t readWord(uint16_t offset, uint64_t access_cycle);
    void writeWord(uint16_t offset, uint16_t value, uint16_t lane_mask,
                   uint64_t access_cycle);

    /* Process scheduled compare/overflow events due through this absolute H8
     * cycle, like MAME internal_update(). Non-event counter time remains lazy
     * until a source register handler calls updateCounter. nextDeadline()
     * returns the earliest enabled event, or UINT64_MAX when none is scheduled. */
    void update(uint64_t cycle);
    uint64_t nextDeadline() const noexcept { return m_next_deadline; }

private:
    struct Channel {
        uint16_t tgr[4];
        uint16_t tcnt;
        uint64_t last_clock_update;
        uint64_t event_time;
        uint32_t phase;
        uint32_t counter_cycle;
        uint8_t tcr, tier, ier, isr;
        int clock_divider;
        int tgr_clearing;
        bool active;
    };

    void setEventTime(Channel &channel, uint64_t when) noexcept;
    void updateCounter(unsigned channel, uint64_t cycle);
    void recalcEvent(unsigned channel, uint64_t cycle, bool request_reschedule);
    void setEnable(unsigned channel, bool enable, uint64_t cycle);
    void raiseIrq(unsigned channel, unsigned flag);
    uint8_t readByteAt(uint16_t offset, uint64_t access_cycle);
    void writeByteAt(uint16_t offset, uint8_t value, uint64_t access_cycle);
    int channelForOffset(uint16_t offset) const noexcept;
    void setTcr(unsigned channel, uint8_t value, uint64_t cycle);
    void setTier(unsigned channel, uint8_t value, uint64_t cycle);
    void setTsr(unsigned channel, uint8_t value, uint64_t cycle);

    void *m_opaque;
    IrqCallback m_irq;
    RescheduleCallback m_reschedule;
    uint8_t m_tstr;
    Channel m_channel[5];
    /* Earliest channel event_time, kept current by setEventTime() because the
     * H8 core queries it on every instruction. */
    uint64_t m_next_deadline;
};

} // namespace arcade

#endif
