// SPDX-License-Identifier: BSD-3-Clause
// QSound LLE device logic adapted from MAME qsound.cpp/.h, copyright Vas Crabb.
// Sample region mapping follows MAME dirom.ipp, copyright Olivier Galibert.
#include "qsound.h"
#include <algorithm>
#include <memory>
#include <new>

struct QSound {
    QSoundConfig config;
    DSP16 *dsp = nullptr;
    uint16_t rom_bank = 0, rom_offset = 0, cmd_addr = 0, cmd_data = 0, new_data = 0;
    uint8_t cmd_pending = 0, dsp_ready = 1;
    int16_t samples[2] = {0, 0};
    uint16_t sr = 0, fsr = 0;
    uint8_t ock = 1, old = 1, ready = 0, channel = 0;
    uint32_t clock = 60000000, rate = 24038, region_mask = 0, region_bank = 0, region_banks = 1;
    uint8_t crystal_remainder = 0;
    uint64_t audio_phase = 0, pcm_frames = 0, recovered_words[2] = {0, 0}, sample_reads = 0, commands = 0;
    explicit QSound(const QSoundConfig &c) : config(c) {
        clock = c.clock_hz ? c.clock_hz : 60000000;
        rate = clock / 2496;
        region_mask = c.sample_bytes - 1;
        region_mask |= region_mask >> 1;
        region_mask |= region_mask >> 2;
        region_mask |= region_mask >> 4;
        region_mask |= region_mask >> 8;
        region_mask |= region_mask >> 16;
        region_banks = std::max(1U, c.sample_bytes / 0x1000000U);
    }
    ~QSound() { dsp16_destroy(dsp); }
    uint8_t sample_byte(uint32_t address) const {
        address &= 0xffffff;
        if (config.sample_bytes >= 0x1000000U)
            return config.samples[uint64_t(region_bank) * 0x1000000U + address];
        address &= region_mask;
        return address < config.sample_bytes ? config.samples[address]
                                             : 0; // MAME default unmapped value is low.
    }
    static uint16_t external(void *p, uint16_t address) {
        auto *q = static_cast<QSound *>(p);
        uint8_t byte = q->sample_byte((uint32_t(q->rom_bank) << 16) | q->rom_offset);
        q->rom_bank = (q->rom_bank & 0x8000U) | (address & 0x7fffU); // I/O aperture mirrors bit 15.
        ++q->sample_reads;
        return uint16_t(byte) << 8;
    }
    static void pio_write(void *p, unsigned select, uint16_t value) {
        auto *q = static_cast<QSound *>(p);
        q->rom_bank = (q->rom_bank & 0x7fffU) | uint16_t(select << 15);
        q->rom_offset = value;
    }
    static uint16_t pio_read(void *p, unsigned) {
        auto *q = static_cast<QSound *>(p);
        if (q->cmd_pending) {
            q->cmd_pending = 0;
            dsp16_set_input(q->dsp, DSP16_INPUT_INT, 0);
            q->dsp_ready = 1; // MAME scheduler synchronization completed at this DSP bus boundary.
            return q->cmd_addr;
        }
        return q->cmd_data;
    }
    static void diagnostic(void *p, int severity, uint16_t pc, const char *message) {
        auto *q = static_cast<QSound *>(p);
        if (q->config.diagnostic)
            q->config.diagnostic(q->config.opaque, severity, pc, message);
    }
    static void pin(void *p, unsigned pin, int state) {
        auto *q = static_cast<QSound *>(p);
        if (pin != DSP16_PIN_OCK || bool(state) == bool(q->ock))
            return;
        q->ock = !!state;
        if (!state || !q->dsp)
            return;
        DSP16Status s{};
        dsp16_get_status(q->dsp, &s);
        if (q->ready && !q->fsr && !s.pins[DSP16_PIN_OSE]) {
            q->channel = s.pins[DSP16_PIN_PSEL];
            q->fsr = 0xffff;
        }
        if (q->fsr) {
            q->sr = (q->sr << 1) | (s.pins[DSP16_PIN_DO] ? 1 : 0);
            q->fsr >>= 1;
            if (!q->fsr) {
                q->samples[q->channel] = int16_t(q->sr);
                ++q->recovered_words[q->channel];
                if (q->config.dac_write)
                    q->config.dac_write(q->config.opaque, q->channel, q->samples[q->channel], s.cycles);
            }
        }
        uint8_t old = s.pins[DSP16_PIN_OLD];
        q->ready = q->old && !old;
        q->old = old;
    }
};
extern "C" QSound *qsound_create(const QSoundConfig *c) {
    if (!c || !c->firmware || c->firmware_words != 0x1000 || !c->samples || !c->sample_bytes ||
        (c->clock_hz && c->clock_hz < 2496))
        return nullptr;
    try {
        std::unique_ptr<QSound> q(new QSound(*c));
        DSP16Config d{};
        d.model = DSP16_MODEL_16A;
        d.rom = c->firmware;
        d.rom_words = c->firmware_words;
        d.opaque = q.get();
        d.external_read = QSound::external;
        d.pio_read = QSound::pio_read;
        d.pio_write = QSound::pio_write;
        d.pin_write = QSound::pin;
        d.diagnostic = QSound::diagnostic;
        q->dsp = dsp16_create(&d);
        if (!q->dsp)
            return nullptr;
        return q.release();
    } catch (...) {
        return nullptr;
    }
}
extern "C" void qsound_destroy(QSound *q) { delete q; }
extern "C" void qsound_reset(QSound *q) {
    if (q) {
        q->cmd_pending = 0;
        q->dsp_ready = 1;
        dsp16_set_input(q->dsp, DSP16_INPUT_INT, 0);
        dsp16_reset(q->dsp);
    }
}
extern "C" int qsound_write(QSound *q, unsigned offset, uint8_t data) {
    if (!q)
        return -1;
    switch (offset) {
    case 0:
        q->new_data = (q->new_data & 0xff) | (uint16_t(data) << 8);
        return 0;
    case 1:
        q->new_data = (q->new_data & 0xff00) | data;
        return 0;
    case 2:
        q->dsp_ready = 0;
        q->cmd_addr = data;
        q->cmd_data = q->new_data;
        q->cmd_pending = 1;
        ++q->commands;
        return dsp16_set_input(q->dsp, DSP16_INPUT_INT, 1);
    default:
        if (q->config.diagnostic)
            q->config.diagnostic(q->config.opaque, DSP16_DIAG_WARNING, 0,
                                 "QSound: unknown host write register");
        return -1;
    }
}
extern "C" uint8_t qsound_read(const QSound *q) { return q && q->dsp_ready ? 0x80 : 0; }
extern "C" uint64_t qsound_run_cycles(QSound *q, uint64_t cycles) {
    if (!q)
        return 0;
    uint64_t done = 0;
    while (cycles) {
        uint64_t increment = uint64_t(q->rate) * 2;
        uint64_t boundary = (q->clock - q->audio_phase + increment - 1) / increment;
        uint64_t request = std::min(cycles, boundary), ran = dsp16_run_cycles(q->dsp, request);
        q->audio_phase += ran * increment;
        done += ran;
        cycles -= ran;
        if (q->audio_phase >= q->clock) {
            q->audio_phase -= q->clock;
            ++q->pcm_frames;
            if (q->config.pcm)
                q->config.pcm(q->config.opaque, q->samples[0], q->samples[1], q->pcm_frames);
        }
        if (ran < request)
            break;
    }
    return done;
}
extern "C" uint64_t qsound_run_clocks(QSound *q, uint64_t clocks) {
    if (!q)
        return 0;
    uint64_t cycles = clocks >> 1;
    unsigned remaining = unsigned(clocks & 1) + q->crystal_remainder;
    cycles += remaining >> 1;
    q->crystal_remainder = remaining & 1;
    return qsound_run_cycles(q, cycles);
}
extern "C" int qsound_set_clock(QSound *q, uint32_t clock) {
    if (!q || clock < 2496)
        return -1;
    q->audio_phase = (q->audio_phase * clock) / q->clock;
    q->clock = clock;
    q->rate = clock / 2496;
    return 0;
}
extern "C" int qsound_set_sample_region_bank(QSound *q, unsigned bank) {
    if (!q || q->config.sample_bytes < 0x1000000U)
        return -1;
    q->region_bank = bank % q->region_banks;
    return 0;
}
extern "C" int qsound_get_status(const QSound *q, QSoundStatus *s) {
    if (!q || !s)
        return -1;
    *s = {};
    dsp16_get_status(q->dsp, &s->dsp);
    s->dsp_cycles = s->dsp.cycles;
    s->halted = s->dsp.halted;
    s->pcm_frames = q->pcm_frames;
    std::copy(q->recovered_words, q->recovered_words + 2, s->recovered_words);
    s->sample_reads = q->sample_reads;
    s->commands = q->commands;
    s->clock_hz = q->clock;
    s->sample_rate = q->rate;
    s->sample_region_bank = q->region_bank;
    s->rom_bank = q->rom_bank;
    s->rom_offset = q->rom_offset;
    s->command_address = q->cmd_addr;
    s->command_data = q->cmd_data;
    s->data_latch = q->new_data;
    std::copy(q->samples, q->samples + 2, s->samples);
    s->command_pending = q->cmd_pending;
    s->ready = q->dsp_ready;
    s->crystal_remainder = q->crystal_remainder;
    return 0;
}
extern "C" DSP16 *qsound_get_dsp(QSound *q) { return q ? q->dsp : nullptr; }
