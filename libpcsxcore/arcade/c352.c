// license:BSD-3-Clause
// copyright-holders:R. Belmont, superctr
/*
 * Namco C352 custom PCM chip emulation.
 * Ported from MAME's c352.cpp (R. Belmont, superctr; additional code by
 * cync and the hoot development team). This standalone API takes the final
 * clock divisor; the common System 12 configuration is 25.4016 MHz / 288.
 *
 * BSD 3-Clause License
 * Copyright (c) R. Belmont and superctr
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain this copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce this copyright notice,
 *    this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of contributors
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
#include "c352.h"

#include <stdlib.h>
#include <string.h>

enum {
    C352_FLG_BUSY       = 0x8000,
    C352_FLG_KEYON      = 0x4000,
    C352_FLG_KEYOFF     = 0x2000,
    C352_FLG_LOOPTRG    = 0x1000,
    C352_FLG_LOOPHIST   = 0x0800,
    C352_FLG_FM          = 0x0400,
    C352_FLG_PHASERL    = 0x0200,
    C352_FLG_PHASEFL    = 0x0100,
    C352_FLG_PHASEFR    = 0x0080,
    C352_FLG_LDIR        = 0x0040,
    C352_FLG_LINK        = 0x0020,
    C352_FLG_NOISE       = 0x0010,
    C352_FLG_MULAW       = 0x0008,
    C352_FLG_FILTER      = 0x0004,
    C352_FLG_REVLOOP     = 0x0003,
    C352_FLG_LOOP        = 0x0002,
    C352_FLG_REVERSE     = 0x0001
};

typedef struct {
    uint32_t pos;
    uint32_t counter;
    int16_t sample;
    int16_t last_sample;
    uint16_t vol_f, vol_r;
    uint8_t curr_vol[4];
    uint16_t freq, flags;
    uint16_t wave_bank, wave_start, wave_end, wave_loop;
} C352Voice;

struct C352 {
    const uint8_t *rom;
    size_t rom_size;
    unsigned sample_rate;
    C352Voice voice[32];
    int16_t mulaw[256];
    uint16_t random;
    uint16_t control;
};

static int16_t wrap_s16(int32_t value)
{
    uint16_t bits = (uint16_t)(uint32_t)value;
    int32_t signed_value = bits < 0x8000u ? (int32_t)bits : (int32_t)bits - 0x10000;
    return (int16_t)signed_value;
}

static uint8_t read_byte(const C352 *c, uint32_t address)
{
    address &= 0x00ffffffu;
    return (size_t)address < c->rom_size ? c->rom[address] : 0xff;
}

static void fetch_sample(C352 *c, C352Voice *v)
{
    v->last_sample = v->sample;
    if (v->flags & C352_FLG_NOISE) {
        c->random = (uint16_t)((c->random >> 1) ^ ((uint16_t)-(int16_t)(c->random & 1) & 0xfff6));
        v->sample = (int16_t)c->random;
        return;
    }

    {
        uint8_t raw = read_byte(c, v->pos);
        uint16_t pos = (uint16_t)v->pos;
        v->sample = (v->flags & C352_FLG_MULAW)
                  ? c->mulaw[raw]
                  : wrap_s16((int32_t)(int8_t)raw * 256);

        if ((v->flags & C352_FLG_LOOP) && (v->flags & C352_FLG_REVERSE)) {
            if ((v->flags & C352_FLG_LDIR) && pos == v->wave_loop)
                v->flags &= (uint16_t)~C352_FLG_LDIR;
            else if (!(v->flags & C352_FLG_LDIR) && pos == v->wave_end)
                v->flags |= C352_FLG_LDIR;
            v->pos += (v->flags & C352_FLG_LDIR) ? (uint32_t)-1 : 1;
        } else if (pos == v->wave_end) {
            if ((v->flags & C352_FLG_LINK) && (v->flags & C352_FLG_LOOP)) {
                v->pos = ((uint32_t)v->wave_start << 16) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            } else if (v->flags & C352_FLG_LOOP) {
                v->pos = (v->pos & 0x00ff0000u) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            } else {
                v->flags |= C352_FLG_KEYOFF;
                v->flags &= (uint16_t)~C352_FLG_BUSY;
                v->sample = 0;
            }
        } else {
            v->pos += (v->flags & C352_FLG_REVERSE) ? (uint32_t)-1 : 1;
        }
    }
}

static void ramp_volume(C352Voice *v, unsigned channel, uint8_t target)
{
    int delta = (int)v->curr_vol[channel] - target;
    if (delta)
        v->curr_vol[channel] = (uint8_t)(v->curr_vol[channel] + (delta > 0 ? -1 : 1));
}

C352 *c352_create(const uint8_t *rom, size_t rom_size,
                  uint32_t clock_hz, unsigned divider)
{
    C352 *c;
    int j = 0;
    unsigned i;
    if (!divider)
        return NULL;
    c = (C352 *)calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->rom = rom;
    c->rom_size = rom_size;
    c->sample_rate = (unsigned)(clock_hz / divider);
    for (i = 0; i < 128; ++i) {
        c->mulaw[i] = (int16_t)(j << 5);
        if (i < 16) j += 1;
        else if (i < 24) j += 2;
        else if (i < 48) j += 4;
        else if (i < 100) j += 8;
        else j += 16;
    }
    for (i = 0; i < 128; ++i)
        c->mulaw[i + 128] = (int16_t)((~(uint16_t)c->mulaw[i]) & 0xffe0u);
    c352_reset(c);
    return c;
}

void c352_destroy(C352 *c) { free(c); }

void c352_reset(C352 *c)
{
    if (!c) return;
    memset(c->voice, 0, sizeof(c->voice));
    c->random = 0x1234;
    c->control = 0;
}

static uint16_t *voice_register(C352Voice *v, unsigned reg)
{
    switch (reg) {
    case 0: return &v->vol_f;
    case 1: return &v->vol_r;
    case 2: return &v->freq;
    case 3: return &v->flags;
    case 4: return &v->wave_bank;
    case 5: return &v->wave_start;
    case 6: return &v->wave_end;
    default: return &v->wave_loop;
    }
}

uint16_t c352_read(C352 *c, uint32_t offset)
{
    if (!c) return 0;
    if (offset < 0x100)
        return *voice_register(&c->voice[offset / 8], offset % 8);
    if (offset == 0x200) return c->control;
    return 0;
}

static uint16_t combine_data(uint16_t old, uint16_t data, uint16_t mask)
{
    return (uint16_t)((old & (uint16_t)~mask) | (data & mask));
}

void c352_write(C352 *c, uint32_t offset, uint16_t data, uint16_t mem_mask)
{
    unsigned i;
    if (!c) return;
    if (offset < 0x100) {
        uint16_t *reg = voice_register(&c->voice[offset / 8], offset % 8);
        *reg = combine_data(*reg, data, mem_mask);
    } else if (offset == 0x200) {
        c->control = combine_data(c->control, data, mem_mask);
    } else if (offset == 0x202 && mem_mask == 0xffff) {
        for (i = 0; i < 32; ++i) {
            C352Voice *v = &c->voice[i];
            if (v->flags & C352_FLG_KEYON) {
                v->pos = ((uint32_t)v->wave_bank << 16) | v->wave_start;
                v->sample = v->last_sample = 0;
                v->counter = 0xffff;
                v->flags |= C352_FLG_BUSY;
                v->flags &= (uint16_t)~(C352_FLG_KEYON | C352_FLG_LOOPHIST);
                memset(v->curr_vol, 0, sizeof(v->curr_vol));
            }
            if (v->flags & C352_FLG_KEYOFF) {
                v->flags &= (uint16_t)~(C352_FLG_BUSY | C352_FLG_KEYOFF);
                v->counter = 0xffff;
            }
        }
    }
}

unsigned c352_sample_rate(const C352 *c) { return c ? c->sample_rate : 0; }

void c352_generate(C352 *c, int16_t *out, size_t frames)
{
    size_t i;
    if (!c || !out) return;
    for (i = 0; i < frames; ++i) {
        int32_t mix[4] = { 0, 0, 0, 0 };
        unsigned j;
        for (j = 0; j < 32; ++j) {
            C352Voice *v = &c->voice[j];
            int32_t sample;
            uint32_t next;
            /* An idle voice outputs zero, so it adds nothing to any channel.
             * About 14 of 32 voices are idle in Soul Calibur play; on the
             * in-order Vita CPU this skips 27 instructions per idle voice. */
            if (!(v->flags & C352_FLG_BUSY))
                continue;
            next = v->counter + v->freq;
            if (next & 0x10000u) fetch_sample(c, v);
            if ((next ^ v->counter) & 0x18000u) {
                ramp_volume(v, 0, (uint8_t)(v->vol_f >> 8));
                ramp_volume(v, 1, (uint8_t)v->vol_f);
                ramp_volume(v, 2, (uint8_t)(v->vol_r >> 8));
                ramp_volume(v, 3, (uint8_t)v->vol_r);
            }
            v->counter = next & 0xffffu;
            sample = v->sample;
            /* MAME multiplies in u32 (counter is u32) and stores into s16.
             * A signed product overflows for large steps and the missing
             * 16-bit truncation then shifts the sample by 65536: a click. */
            if (!(v->flags & C352_FLG_FILTER))
                sample = (int16_t)(uint16_t)((uint32_t)v->last_sample +
                    (v->counter * (uint32_t)(v->sample - v->last_sample) >> 16));
            mix[0] += (((v->flags & C352_FLG_PHASEFL) ? -sample : sample) * v->curr_vol[0]) >> 8;
            mix[2] += (((v->flags & C352_FLG_PHASERL) ? -sample : sample) * v->curr_vol[2]) >> 8;
            mix[1] += (((v->flags & C352_FLG_PHASEFR) ? -sample : sample) * v->curr_vol[1]) >> 8;
            mix[3] += (((v->flags & C352_FLG_PHASEFR) ? -sample : sample) * v->curr_vol[3]) >> 8;
        }
        out[i * 4 + 0] = wrap_s16(mix[0] >> 3);
        out[i * 4 + 1] = wrap_s16(mix[1] >> 3);
        out[i * 4 + 2] = wrap_s16(mix[2] >> 3);
        out[i * 4 + 3] = wrap_s16(mix[3] >> 3);
    }
}
