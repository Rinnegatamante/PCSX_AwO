/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Olivier Galibert (original MAME implementation).
 * Copyright (c) 2026 (standalone port).
 * See the BSD 3-Clause License text below.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef PCSX_ARCADE_H8_ADC_H
#define PCSX_ARCADE_H8_ADC_H

#include <cstdint>

namespace arcade {

/* Standalone H8/3337 ADC. The caller supplies absolute H8 cycle timestamps,
 * invokes update() at the returned deadline, and routes IRQ requests to the
 * appropriate H8 interrupt vector. Byte offsets 0..7 are the result registers,
 * 8 is ADCSR, and 9 is ADCR (the FFE0..FFE9 window). */
class H8Adc3337 {
public:
    typedef uint16_t (*AnalogRead)(void *opaque, unsigned channel);
    typedef void (*IrqRequest)(void *opaque);
    typedef void (*Reschedule)(void *opaque);

    H8Adc3337(void *opaque = 0, AnalogRead analog_read = 0,
              IrqRequest irq_request = 0);

    void setCallbacks(void *opaque, AnalogRead analog_read, IrqRequest irq_request,
                      Reschedule reschedule = 0);
    void reset(uint64_t cycle = 0);
    uint8_t read(uint8_t offset) const;
    void write(uint8_t offset, uint8_t data, uint64_t cycle);
    void trigger(bool level, uint64_t cycle);
    uint64_t update(uint64_t cycle);

    uint64_t next_event() const { return m_next_event; }

private:
    enum { MODE_ACTIVE = 1, MODE_REPEAT = 2, MODE_ROTATE = 4 };

    void update_mode();
    void start_conversion(uint64_t cycle);
    void sample();
    void commit();
    void timeout(uint64_t cycle);
    unsigned first_time() const;
    unsigned subsequent_time() const;

    void *m_opaque;
    AnalogRead m_analog_read;
    IrqRequest m_irq_request;
    Reschedule m_reschedule;
    uint16_t m_addr[4];
    uint16_t m_sample;
    uint8_t m_adcsr;
    uint8_t m_adcr;
    uint8_t m_trigger;
    uint8_t m_start_mode;
    uint8_t m_mode;
    uint8_t m_start_channel;
    uint8_t m_end_channel;
    uint8_t m_channel;
    bool m_trigger_level;
    uint64_t m_next_event;
};

} // namespace arcade

#endif
