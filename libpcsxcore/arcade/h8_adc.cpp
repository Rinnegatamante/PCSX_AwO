/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Olivier Galibert (original MAME implementation).
 * Copyright (c) 2026 (standalone port).
 * Derived from MAME's h8_adc.cpp by Olivier Galibert, at
 * https://github.com/mamedev/mame/blob/deb2f54fe7300e7a192690318beb860159bfd52a/src/devices/cpu/h8/h8_adc.cpp
 * MAME source is distributed under the BSD 3-Clause License.
 */
#include "h8_adc.h"
#include <cstring>

namespace arcade {

H8Adc3337::H8Adc3337(void *opaque, AnalogRead analog_read, IrqRequest irq_request)
    : m_opaque(opaque), m_analog_read(analog_read), m_irq_request(irq_request),
      m_reschedule(0) {
    reset();
}

void H8Adc3337::setCallbacks(void *opaque, AnalogRead analog_read,
                             IrqRequest irq_request, Reschedule reschedule) {
    m_opaque = opaque;
    m_analog_read = analog_read;
    m_irq_request = irq_request;
    m_reschedule = reschedule;
}

void H8Adc3337::reset(uint64_t) {
    std::memset(m_addr, 0, sizeof(m_addr));
    m_sample = 0;
    m_adcsr = 0;
    m_adcr = 0;
    m_trigger = 1; // T_SOFT
    m_start_mode = 0;
    m_mode = 0;
    m_start_channel = 0;
    m_end_channel = 0;
    m_channel = 0;
    m_trigger_level = true;
    m_next_event = 0;
    update_mode();
}

uint8_t H8Adc3337::read(uint8_t offset) const {
    if (offset < 8) {
        const uint16_t value = m_addr[offset >> 1];
        return (offset & 1) ? static_cast<uint8_t>(value << 6)
                            : static_cast<uint8_t>(value >> 2);
    }
    if (offset == 8)
        return m_adcsr;
    if (offset == 9)
        return m_adcr;
    return 0xff;
}

void H8Adc3337::write(uint8_t offset, uint8_t data, uint64_t cycle) {
    if (offset == 8) {
        const uint8_t previous = m_adcsr;
        // MAME's ADF is cleared by writing zero and retained by writing one.
        m_adcsr = static_cast<uint8_t>((data & 0x7f) | (m_adcsr & data & 0x80));
        update_mode();
        if (!(previous & 0x20) && (m_adcsr & 0x20))
            start_conversion(cycle);
    } else if (offset == 9) {
        m_adcr = data;
        update_mode();
    }
}

void H8Adc3337::trigger(bool level, uint64_t cycle) {
    if (m_trigger_level != level) {
        m_trigger_level = level;
        if (!level && (m_trigger & 0x08) && !(m_adcsr & 0x20)) {
            m_adcsr |= 0x20;
            start_conversion(cycle);
        }
    }
}

uint64_t H8Adc3337::update(uint64_t cycle) {
    if (m_next_event && m_next_event <= cycle) {
        m_next_event = 0;
        timeout(cycle);
    }
    return m_next_event;
}

void H8Adc3337::update_mode() {
    m_trigger = (m_adcr & 0x80) ? 0x08 : 0x01; // T_EXT or T_SOFT
    if (m_adcsr & 0x10) {
        m_start_mode = MODE_ACTIVE | MODE_REPEAT | MODE_ROTATE;
        m_start_channel = m_adcsr & 0x04;
        m_end_channel = m_adcsr & 0x07;
    } else {
        m_start_mode = MODE_ACTIVE;
        m_start_channel = m_end_channel = m_adcsr & 0x07;
    }
}

void H8Adc3337::start_conversion(uint64_t cycle) {
    m_mode = m_start_mode;
    m_channel = m_start_channel;
    sample();
    m_next_event = cycle + first_time();
    // MAME conversion_wait() asks the CPU to recompute its event bound when
    // a register/pin starts a conversion; scan timeouts supply their own time.
    if (m_reschedule)
        m_reschedule(m_opaque);
}

void H8Adc3337::sample() {
    m_sample = m_analog_read
        ? static_cast<uint16_t>(m_analog_read(m_opaque, m_channel) & 0x03ff)
        : 0;
}

void H8Adc3337::commit() {
    m_addr[m_channel & 3] = m_sample;
}

void H8Adc3337::timeout(uint64_t cycle) {
    if (!(m_mode & MODE_ACTIVE))
        return;

    commit();
    if ((m_mode & MODE_ROTATE) && m_channel != m_end_channel) {
        m_channel = (m_channel + 1) & 7;
        sample();
        m_next_event = cycle + subsequent_time();
        return;
    }
    if (m_mode & MODE_ROTATE)
        m_channel = m_start_channel;

    m_adcsr |= 0x80;
    if ((m_adcsr & 0x40) && m_irq_request)
        m_irq_request(m_opaque);

    if (m_mode & MODE_REPEAT) {
        m_channel = m_start_channel;
        sample();
        m_next_event = cycle + subsequent_time();
        return;
    }

    m_mode = 0;
    m_adcsr &= static_cast<uint8_t>(~0x20);
}

unsigned H8Adc3337::first_time() const {
    return (m_adcsr & 0x08) ? 134 : 266;
}

unsigned H8Adc3337::subsequent_time() const {
    return (m_adcsr & 0x08) ? 128 : 256;
}

} // namespace arcade
