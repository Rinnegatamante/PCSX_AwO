/* SPDX-License-Identifier: BSD-3-Clause
 * copyright-holders:Olivier Galibert, R. Belmont, hap, superctr
 * Standalone C99 adaptation of src/devices/sound/zsg2.cpp/.h, MAME commit
 * deb2f54fe7300e7a192690318beb860159bfd52a. See zsg2.LICENSE for full terms.
 * Written by Olivier Galibert; MAME conversion R. Belmont; working emulation
 * hap; properly working emulation superctr (upstream attribution).
 */
#include "zsg2.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct ZSG2 {
    ZSG2Config config;
    ZSG2Channel channel[ZSG2_CHANNELS];
    uint16_t gain[32], control[32];
    uint32_t sample_count, read_address, rom_words, diagnostics;
    uint32_t *cache_words;
    int16_t *decoded;
    uint64_t cycles, frames, sample_phase, periodic_phase, diagnostic_events;
    uint32_t sample_rate;
    int pending_batch, busy;
};
/* Define two's-complement narrowing and arithmetic shifts without signed UB. */
static int16_t signed16(uint16_t v)
{ return (int16_t)(v <= INT16_MAX ? (int32_t)v : (int32_t)v - 65536); }
static int32_t signed32(uint32_t v)
{ return v <= INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX - v); }
static int32_t asr32(int32_t value, unsigned bits)
{
    if (!bits) return value;
    return signed32(((uint32_t)value >> bits) | (value < 0 ? UINT32_MAX << (32 - bits) : 0));
}
static int32_t wrap32(int64_t v) { return signed32((uint32_t)v); }
static int16_t clip16(int32_t v)
{ return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }
static void diagnostic(ZSG2 *d, uint32_t code, uint32_t offset, uint16_t value)
{
    d->diagnostics |= code; d->diagnostic_events++;
    if (d->config.diagnostic) {
        int was_busy = d->busy;
        d->busy = 1;
        d->config.diagnostic(d->config.diagnostic_opaque, code, offset, value);
        d->busy = was_busy;
    }
}
static uint32_t read_memory(ZSG2 *d, uint32_t offset)
{
    const uint8_t *p;
    if (offset >= d->rom_words) return 0;
    if (d->config.external_read) {
        int was_busy = d->busy;
        uint32_t value;
        d->busy = 1;
        value = d->config.external_read(d->config.rom_opaque, offset);
        d->busy = was_busy;
        return value;
    }
    p = d->config.sample_rom + (size_t)offset * 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int16_t *prepare_samples(ZSG2 *d, uint32_t offset)
{
    uint32_t block = read_memory(d, offset), values[4]; unsigned i, shift;
    /* Literal source alias: mem_blocks is an int16 index, not mem_blocks*4. */
    if (!block) return d->decoded + d->rom_words;
    if (block == d->cache_words[offset]) return d->decoded + (size_t)offset * 4;
    d->cache_words[offset] = block;
    values[0] = (block >> 8) & 0x7f; values[1] = (block >> 16) & 0x7f;
    values[2] = (block >> 24) & 0x7f;
    values[3] = ((block >> 9) & 0x40) | ((block >> 18) & 0x20) | ((block >> 27) & 0x10) | (block & 15);
    shift = (block >> 4) & 15;
    for (i = 0; i < 4; i++)
        d->decoded[(size_t)offset * 4 + i] = (int16_t)asr32(signed16((uint16_t)(values[i] << 9)), shift);
    return d->decoded + (size_t)offset * 4;
}
static void filter_samples(ZSG2 *d, ZSG2Channel *c)
{
    int16_t *raw = prepare_samples(d, c->page | c->current_position); unsigned i;
    c->samples[0] = c->samples[4];
    for (i = 0; i < 4; i++) {
        c->emphasis_filter = wrap32((int64_t)c->emphasis_filter + raw[i] - asr32(wrap32((int64_t)c->emphasis_filter + 0x20), 6));
        c->samples[i + 1] = clip16(asr32(c->emphasis_filter, 1));
    }
}
static int16_t get_ramp(uint8_t value)
{
    int32_t fraction = asr32(signed16((uint16_t)((uint32_t)value << 12)), 12);
    fraction = signed32((uint32_t)fraction ^ 8u);
    fraction = signed16((uint16_t)(fraction * (int32_t)(1u << (value >> 4))));
    return (int16_t)asr32(fraction, 4);
}
static uint16_t ramp(uint16_t current, uint16_t target, int16_t delta)
{
    int32_t result = (int32_t)current + delta;
    if ((delta < 0 && result < target) || (delta >= 0 && result > target)) result = target;
    return (uint16_t)result;
}
static void render_frame(ZSG2 *d, int16_t out[4])
{
    int32_t mix[4] = {0}; unsigned ch, output;
    for (ch = 0; ch < ZSG2_CHANNELS; ch++) {
        ZSG2Channel *c = &d->channel[ch]; int32_t sample; unsigned pos; int16_t difference;
        if (!(c->status & ZSG2_ACTIVE)) continue;
        c->step_pointer += c->step;
        if (c->step_pointer & 0xffff0000u) {
            if (++c->current_position >= c->end) {
                c->current_position = c->loop;
                if (c->current_position + 1 >= c->end) { c->volume = 0; c->status &= ~ZSG2_ACTIVE; continue; }
            }
            if (c->current_position == c->start) c->emphasis_filter = 0;
            c->step_pointer &= 0xffff; filter_samples(d, c);
        }
        pos = (c->step_pointer >> 14) & 3; sample = c->samples[pos];
        difference = signed16((uint16_t)(c->samples[pos + 1] - sample));
        sample += asr32((int32_t)(((c->step_pointer << 2) & 0xffff) * (int64_t)difference), 16);
        c->output_filter = wrap32((int64_t)c->output_filter + (int64_t)(sample - asr32(c->output_filter, 16)) * c->cutoff);
        sample = asr32(c->output_filter, 16);
        if (!c->cutoff) c->output_filter = asr32(c->output_filter, 1);
        sample = asr32(sample * (int32_t)c->volume, 16);
        for (output = 0; output < 4; output++) {
            int32_t routed = c->output_gain[output] & 0x80 ? -sample : sample;
            mix[output] += asr32(routed * d->gain[c->output_gain[output] & 31], 16);
        }
        /* Literal upstream parity is per stream-update batch, not per frame. */
        if (d->sample_count & 1) {
            c->volume = ramp(c->volume, c->target_volume, c->volume_delta);
            c->cutoff = ramp(c->cutoff, c->target_cutoff, c->cutoff_delta);
        }
    }
    for (output = 0; output < 4; output++) out[output] = clip16(mix[output]);
    d->frames++; d->pending_batch = 1;
    if (d->config.sample) d->config.sample(d->config.sample_opaque, out, d->cycles);
}
static void chan_write(ZSG2 *d, unsigned ch, unsigned reg, uint16_t data)
{
    ZSG2Channel *c = &d->channel[ch];
    switch (reg) {
    case 0: c->start = (c->start & 0xff00) | ((data >> 8) & 255); break;
    case 1: c->start = (c->start & 255) | (((uint32_t)data << 8) & 0xff00); c->page = ((uint32_t)data << 8) & 0xff0000; break;
    case 2: break;
    case 3: c->status = (c->status & ZSG2_ACTIVE) | (data & 0x7fff); break;
    case 4: c->step = (uint32_t)data + 1; break;
    case 5: c->loop = (c->loop & 0xff00) | (data & 255); c->output_gain[3] = data >> 8; break;
    case 6: c->end = data; break;
    case 7: c->loop = (c->loop & 255) | (((uint32_t)data << 8) & 0xff00); c->output_gain[2] = data >> 8; break;
    case 8: c->initial_cutoff = data; break;
    case 9: c->cutoff = data; break;
    case 10: c->initial_volume = data; break;
    case 11: c->volume = data; break;
    case 12: c->target_cutoff = data; break;
    case 13: c->output_gain[1] = data >> 8; c->cutoff_delta = get_ramp(data & 255); break;
    case 14: c->target_volume = data; break;
    case 15: c->output_gain[0] = data >> 8; c->volume_delta = get_ramp(data & 255); break;
    }
    c->registers[reg] = data;
}
static uint16_t chan_read(const ZSG2 *d, unsigned ch, unsigned reg)
{
    const ZSG2Channel *c = &d->channel[ch];
    if (reg == 3) return c->status;
    if (reg == 9) return c->cutoff;
    if (reg == 11) return c->volume;
    return c->registers[reg];
}
static void control_write(ZSG2 *d, uint32_t reg, uint16_t data)
{
    unsigned i;
    switch (reg) {
    case 0: case 1: case 2:
        for (i = 0; i < 16; i++) if (data & (1u << i)) {
            ZSG2Channel *c = &d->channel[(reg & 3) * 16 + i];
            c->status |= ZSG2_ACTIVE; c->current_position = c->start - 1; c->step_pointer = 0x10000;
            c->volume = 0; c->volume_delta = 0x400; c->cutoff = c->initial_cutoff; c->output_filter = 0;
        }
        break;
    case 4: case 5: case 6:
        for (i = 0; i < 16; i++) if (data & (1u << i)) {
            ZSG2Channel *c = &d->channel[(reg & 3) * 16 + i];
            c->volume = 0; c->status &= ~ZSG2_ACTIVE;
        }
        break;
    case 0x1c:
        if (data & 3) diagnostic(d, ZSG2_DIAG_UNALIGNED_ROM_ADDRESS, 0x300 + reg, data);
        d->read_address = (d->read_address & 0x3fffc000) | ((data >> 2) & 0x3fff); break;
    case 0x1d:
        d->read_address = (d->read_address & 0x3fff) | (((uint32_t)data << 14) & 0x3fffc000); break;
    default:
        if (reg < 32) d->control[reg] = data;
        diagnostic(d, ZSG2_DIAG_UNKNOWN_CONTROL, 0x300 + reg, data); break;
    }
}
static uint16_t control_read(ZSG2 *d, uint32_t reg)
{
    switch (reg) {
    case 0x14: return 0; /* Literal upstream "memory bus busy?" behavior. */
    case 0x1e: return (uint16_t)read_memory(d, d->read_address);
    case 0x1f: return read_memory(d, d->read_address) >> 16;
    default: return reg < 32 ? d->control[reg] : 0;
    }
}
int zsg2_create(const ZSG2Config *config, ZSG2 **out)
{
    ZSG2 *d; size_t words; unsigned i;
    if (out) *out = NULL;
    if (!out || !config || config->clock_hz < 768 || !config->sample_rom_size || (config->sample_rom_size & 3) ||
        (!config->sample_rom && !config->external_read)) return ZSG2_ARGUMENT;
    words = config->sample_rom_size / 4;
    if (words > UINT32_MAX || words > (SIZE_MAX / sizeof(int16_t) - 4) / 4) return ZSG2_ARGUMENT;
    d = calloc(1, sizeof(*d)); if (!d) return ZSG2_NOMEM;
    d->config = *config; d->rom_words = (uint32_t)words; d->sample_rate = config->clock_hz / 768;
    d->cache_words = calloc(words, sizeof(uint32_t)); d->decoded = calloc(words * 4 + 4, sizeof(int16_t));
    if (!d->cache_words || !d->decoded) { zsg2_destroy(d); return ZSG2_NOMEM; }
    for (i = 1; i < 32; i++) d->gain[i] = (uint16_t)(pow(10.0, -(31 - (int)i) / 20.0) * 65535.0);
    zsg2_reset(d); *out = d; return ZSG2_OK;
}
void zsg2_destroy(ZSG2 *d)
{ if (d && !d->busy) { free(d->cache_words); free(d->decoded); free(d); } }
int zsg2_reset(ZSG2 *d)
{
    unsigned ch, reg;
    if (!d) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    d->read_address = 0;
    control_write(d, 4, 0xffff); control_write(d, 5, 0xffff); control_write(d, 6, 0xffff);
    for (ch = 0; ch < ZSG2_CHANNELS; ch++) for (reg = 0; reg < 16; reg++) chan_write(d, ch, reg, 0);
    d->sample_count = 0; d->pending_batch = 0;
    return ZSG2_OK;
}
int zsg2_stream_update(ZSG2 *d)
{
    if (!d) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    if (d->pending_batch) { d->sample_count++; d->pending_batch = 0; }
    return ZSG2_OK;
}
int zsg2_read(ZSG2 *d, uint32_t offset, uint16_t mask, uint16_t *value)
{
    if (!d || !value) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    if (mask != 0xffff) { *value = 0; diagnostic(d, ZSG2_DIAG_PARTIAL_ACCESS, offset, mask); return ZSG2_MASK_UNSUPPORTED; }
    zsg2_stream_update(d);
    *value = offset < 0x300 ? chan_read(d, offset >> 4, offset & 15) : control_read(d, offset - 0x300);
    return ZSG2_OK;
}
int zsg2_write(ZSG2 *d, uint32_t offset, uint16_t value, uint16_t mask)
{
    if (!d) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    if (mask != 0xffff) { diagnostic(d, ZSG2_DIAG_PARTIAL_ACCESS, offset, mask); return ZSG2_MASK_UNSUPPORTED; }
    zsg2_stream_update(d);
    if (offset < 0x300) chan_write(d, offset >> 4, offset & 15, value); else control_write(d, offset - 0x300, value);
    return ZSG2_OK;
}
int zsg2_run_cycles(ZSG2 *d, uint64_t cycles)
{
    uint64_t target;
    if (!d) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    if (cycles > UINT64_MAX - d->cycles) return ZSG2_TIME_OVERFLOW;
    target = d->cycles + cycles;
    while (d->cycles < target) {
        uint64_t frame_due = ((uint64_t)d->config.clock_hz - d->sample_phase + d->sample_rate - 1) / d->sample_rate;
        uint64_t update_due = ((uint64_t)d->config.clock_hz - d->periodic_phase + 49) / 50;
        uint64_t delta = frame_due < update_due ? frame_due : update_due;
        if (delta > target - d->cycles) delta = target - d->cycles;
        d->cycles += delta; d->sample_phase += delta * d->sample_rate; d->periodic_phase += delta * 50;
        if (d->sample_phase >= d->config.clock_hz) {
            int16_t outputs[4];
            d->sample_phase -= d->config.clock_hz; d->busy = 1; render_frame(d, outputs); d->busy = 0;
        }
        /* Include frame due at this boundary before closing the source batch. */
        if (d->periodic_phase >= d->config.clock_hz) {
            d->periodic_phase -= d->config.clock_hz; zsg2_stream_update(d);
        }
    }
    return ZSG2_OK;
}
int zsg2_advance_to_cycle(ZSG2 *d, uint64_t cycle)
{
    if (!d) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    return cycle < d->cycles ? ZSG2_TIME_REWIND : zsg2_run_cycles(d, cycle - d->cycles);
}
int zsg2_render_update(ZSG2 *d, size_t frames, int16_t *outputs, size_t capacity)
{
    size_t i;
    if (!d) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    if (outputs && capacity < frames) return ZSG2_CAPACITY;
    if (frames > UINT64_MAX - d->frames || frames > SIZE_MAX / 4) return ZSG2_TIME_OVERFLOW;
    d->busy = 1;
    for (i = 0; i < frames; i++) { int16_t frame[4]; render_frame(d, frame); if (outputs) memcpy(outputs + i * 4, frame, sizeof(frame)); }
    d->busy = 0; d->sample_count++; d->pending_batch = 0;
    return ZSG2_OK;
}
void zsg2_get_status(const ZSG2 *d, ZSG2Status *s)
{
    if (!d || !s) return;
    s->clock_hz = d->config.clock_hz; s->sample_rate = d->sample_rate; s->rom_words = d->rom_words;
    s->read_address = d->read_address; s->sample_count = d->sample_count; s->diagnostics = d->diagnostics;
    s->diagnostic_events = d->diagnostic_events; s->cycles = d->cycles; s->frames = d->frames;
    s->sample_phase = d->sample_phase; s->periodic_phase = d->periodic_phase; s->pending_batch = d->pending_batch;
}
int zsg2_get_channel(const ZSG2 *d, unsigned channel, ZSG2Channel *state)
{
    if (!d || !state || channel >= ZSG2_CHANNELS) return ZSG2_ARGUMENT;
    *state = d->channel[channel]; return ZSG2_OK;
}
int zsg2_decode_block(ZSG2 *d, uint32_t word_offset, int16_t samples[4])
{
    if (!d || !samples) return ZSG2_ARGUMENT;
    if (d->busy) return ZSG2_REENTRANT;
    memcpy(samples, prepare_samples(d, word_offset), 4 * sizeof(int16_t)); return ZSG2_OK;
}
