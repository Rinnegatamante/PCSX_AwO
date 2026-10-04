/* license:BSD-3-Clause; copyright-holders:Olivier Galibert */
/* Source-faithful standalone K054539 engine port from MAME k054539.cpp.
 * Full BSD notice: k054539_LICENSE.txt. */
#include "k054539.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct K054539Channel {
    uint32_t pos;
    uint32_t pfrac;
    int32_t val;
    int32_t pval;
} K054539Channel;

typedef union K054539ReverbRam {
    uint8_t bytes[K054539_REVERB_BYTES];
    int16_t words[K054539_REVERB_BYTES / 2];
} K054539ReverbRam;

struct K054539 {
    uint32_t clock_hz;
    uint32_t sample_rate;
    uint64_t cycles;
    uint8_t regs[K054539_REGISTER_BYTES];
    uint8_t posreg_latch[K054539_VOICES][3];
    K054539ReverbRam reverb_ram;
    int16_t reverb_pos;
    uint32_t cur_ptr;
    uint32_t rom_addr;
    K054539Channel channels[K054539_VOICES];
    double voltab[256];
    double pantab[15];
    double gain[K054539_VOICES];
    int flags;
    uint8_t timer_state;
    int timer_active;
    float timer_frequency;
    double timer_period_cycles;
    double timer_next_cycle;
    K054539SampleRead sample_read;
    K054539TimerLine timer_line;
    K054539AnalogPan analog_pan;
    void *opaque;
    unsigned callback_depth;
    int callback_fault;
};

static int mutation_allowed(K054539 *chip)
{
    if (chip->callback_depth != 0) {
        chip->callback_fault = 1;
        return 0;
    }
    return 1;
}

static int32_t signed16(uint16_t value)
{
    return (value & 0x8000u) ? (int32_t)value - 0x10000 : (int32_t)value;
}

static uint8_t sample_byte(K054539 *chip, uint32_t address)
{
    uint8_t value;
    ++chip->callback_depth;
    value = chip->sample_read(chip->opaque, address & 0x00ffffffu);
    --chip->callback_depth;
    return value;
}

static int regupdate(const K054539 *chip)
{
    return !(chip->regs[0x22f] & 0x80);
}

static void timer_line(K054539 *chip, int state)
{
    if (chip->timer_line != NULL) {
        ++chip->callback_depth;
        chip->timer_line(chip->opaque, chip->cycles, state);
        --chip->callback_depth;
    }
}

static void keyon(K054539 *chip, unsigned channel)
{
    if (regupdate(chip))
        chip->regs[0x22c] |= (uint8_t)(1u << channel);
}

static void keyoff(K054539 *chip, unsigned channel)
{
    if (regupdate(chip))
        chip->regs[0x22c] &= (uint8_t)~(1u << channel);
}

static int32_t trunc_s32(double value)
{
    /* MAME put_int accepts s32, so its implicit conversion truncates toward
     * zero before the resulting integer is normalized into sample_t. */
    if (value >= 2147483647.0)
        return INT32_MAX;
    if (value <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)value;
}

static void render_sample(K054539 *chip, K054539SampleSink sink,
                          void *sink_opaque)
{
    static const int16_t dpcm[16] = {
        0 * 0x100, 1 * 0x100, 2 * 0x100, 4 * 0x100,
        8 * 0x100, 16 * 0x100, 32 * 0x100, 64 * 0x100,
        0 * 0x100, -64 * 0x100, -32 * 0x100, -16 * 0x100,
        -8 * 0x100, -4 * 0x100, -2 * 0x100, -1 * 0x100
    };
    double lval = 0.0;
    double rval = 0.0;
    int16_t *rbase = chip->reverb_ram.words;
    unsigned ch;

    if (!(chip->regs[0x22f] & 1)) {
        if (sink != NULL) {
            ++chip->callback_depth;
            sink(sink_opaque, chip->cycles, 0, 0);
            --chip->callback_depth;
        }
        return;
    }

    if (!(chip->flags & K054539_FLAG_DISABLE_REVERB))
        lval = rval = rbase[chip->reverb_pos];
    rbase[chip->reverb_pos] = 0;

    for (ch = 0; ch < K054539_VOICES; ++ch) {
        if (chip->regs[0x22c] & (1u << ch)) {
            uint8_t *base1 = chip->regs + 0x20 * ch;
            uint8_t *base2 = chip->regs + 0x200 + 2 * ch;
            K054539Channel *channel = &chip->channels[ch];
            int32_t delta = base1[0] | ((int32_t)base1[1] << 8) |
                            ((int32_t)base1[2] << 16);
            int vol = base1[3];
            int bval = vol + base1[4];
            int pan = base1[5];
            double lvol, rvol, rbvol;
            int rdelta;
            int32_t cur_pos = (int32_t)(base1[0x0c] |
                                        ((uint32_t)base1[0x0d] << 8) |
                                        ((uint32_t)base1[0x0e] << 16));
            int32_t fdelta, pdelta;
            int32_t cur_pfrac, cur_val, cur_pval;

            if (bval > 255)
                bval = 255;
            if (pan >= 0x81 && pan <= 0x8f)
                pan -= 0x81;
            else if (pan >= 0x11 && pan <= 0x1f)
                pan -= 0x11;
            else
                pan = 0x18 - 0x11;

            lvol = chip->voltab[vol] * chip->pantab[pan] * chip->gain[ch];
            if (lvol > 1.80)
                lvol = 1.80;
            rvol = chip->voltab[vol] * chip->pantab[0xe - pan] * chip->gain[ch];
            if (rvol > 1.80)
                rvol = 1.80;
            rbvol = chip->voltab[bval] * chip->gain[ch] / 2.0;
            if (rbvol > 1.80)
                rbvol = 1.80;

            rdelta = (base1[6] | ((int)base1[7] << 8)) >> 3;
            rdelta = (rdelta + chip->reverb_pos) & 0x3fff;

            if (base2[0] & 0x20) {
                delta = -delta;
                fdelta = 0x10000;
                pdelta = -1;
            } else {
                fdelta = -0x10000;
                pdelta = 1;
            }

            if ((uint32_t)cur_pos != channel->pos) {
                channel->pos = (uint32_t)cur_pos;
                cur_pfrac = 0;
                cur_val = 0;
                cur_pval = 0;
            } else {
                cur_pfrac = (int32_t)channel->pfrac;
                cur_val = channel->val;
                cur_pval = channel->pval;
            }

            switch (base2[0] & 0x0c) {
            case 0x00: /* 8-bit signed PCM */
                cur_pfrac += delta;
                while (cur_pfrac & ~0xffff) {
                    uint8_t raw;
                    cur_pfrac += fdelta;
                    cur_pos += pdelta;
                    cur_pval = cur_val;
                    raw = sample_byte(chip, (uint32_t)cur_pos);
                    cur_val = signed16((uint16_t)((uint16_t)raw << 8));
                    if (cur_val == -32768 && (base2[1] & 1)) {
                        cur_pos = (int32_t)(base1[8] |
                                            ((uint32_t)base1[9] << 8) |
                                            ((uint32_t)base1[10] << 16));
                        raw = sample_byte(chip, (uint32_t)cur_pos);
                        cur_val = signed16((uint16_t)((uint16_t)raw << 8));
                    }
                    if (cur_val == -32768) {
                        keyoff(chip, ch);
                        cur_val = 0;
                        break;
                    }
                }
                break;

            case 0x04: /* 16-bit little-endian signed PCM */
                pdelta *= 2;
                cur_pfrac += delta;
                while (cur_pfrac & ~0xffff) {
                    uint16_t raw;
                    cur_pfrac += fdelta;
                    cur_pos += pdelta;
                    cur_pval = cur_val;
                    raw = (uint16_t)(sample_byte(chip, (uint32_t)cur_pos) |
                        ((uint16_t)sample_byte(chip, (uint32_t)(cur_pos + 1)) << 8));
                    cur_val = signed16(raw);
                    if (cur_val == -32768 && (base2[1] & 1)) {
                        cur_pos = (int32_t)(base1[8] |
                                            ((uint32_t)base1[9] << 8) |
                                            ((uint32_t)base1[10] << 16));
                        raw = (uint16_t)(sample_byte(chip, (uint32_t)cur_pos) |
                            ((uint16_t)sample_byte(chip, (uint32_t)(cur_pos + 1)) << 8));
                        cur_val = signed16(raw);
                    }
                    if (cur_val == -32768) {
                        keyoff(chip, ch);
                        cur_val = 0;
                        break;
                    }
                }
                break;

            case 0x08: /* 4-bit delta PCM */
                cur_pos <<= 1;
                cur_pfrac <<= 1;
                if (cur_pfrac & 0x10000) {
                    cur_pfrac &= 0xffff;
                    cur_pos |= 1;
                }
                cur_pfrac += delta;
                while (cur_pfrac & ~0xffff) {
                    uint8_t raw;
                    cur_pfrac += fdelta;
                    cur_pos += pdelta;
                    cur_pval = cur_val;
                    raw = sample_byte(chip, (uint32_t)(cur_pos >> 1));
                    cur_val = raw;
                    if (cur_val == 0x88 && (base2[1] & 1)) {
                        cur_pos = (int32_t)((base1[8] |
                                            ((uint32_t)base1[9] << 8) |
                                            ((uint32_t)base1[10] << 16)) << 1);
                        cur_val = sample_byte(chip, (uint32_t)(cur_pos >> 1));
                    }
                    if (cur_val == 0x88) {
                        keyoff(chip, ch);
                        cur_val = 0;
                        break;
                    }
                    if (cur_pos & 1)
                        cur_val >>= 4;
                    else
                        cur_val &= 15;
                    cur_val = cur_pval + dpcm[cur_val];
                    if (cur_val < -32768)
                        cur_val = -32768;
                    else if (cur_val > 32767)
                        cur_val = 32767;
                }
                cur_pfrac >>= 1;
                if (cur_pos & 1)
                    cur_pfrac |= 0x8000;
                cur_pos >>= 1;
                break;

            default:
                /* Upstream leaves channel state unchanged for reserved type. */
                break;
            }

            lval += cur_val * lvol;
            rval += cur_val * rvol;
            {
                unsigned out = (unsigned)((rdelta + chip->reverb_pos) & 0x1fff);
                int32_t wide_add = trunc_s32(cur_val * rbvol);
                uint16_t add = (uint16_t)(uint32_t)wide_add;
                uint16_t sum = (uint16_t)rbase[out] + add;
                rbase[out] = signed16(sum);
            }

            channel->pos = (uint32_t)cur_pos;
            channel->pfrac = (uint32_t)cur_pfrac;
            channel->pval = cur_pval;
            channel->val = cur_val;

            if (regupdate(chip)) {
                base1[0x0c] = (uint8_t)cur_pos;
                base1[0x0d] = (uint8_t)(cur_pos >> 8);
                base1[0x0e] = (uint8_t)(cur_pos >> 16);
            }
        }
    }

    chip->reverb_pos = (int16_t)((chip->reverb_pos + 1) & 0x1fff);
    if (sink != NULL) {
        ++chip->callback_depth;
        sink(sink_opaque, chip->cycles, trunc_s32(lval), trunc_s32(rval));
        --chip->callback_depth;
    }
}

K054539 *k054539_create(const K054539Config *config)
{
    K054539 *chip;
    unsigned i;
    if (config == NULL || config->clock_hz == 0 ||
        config->clock_hz % K054539_CLOCK_DIVIDER != 0 ||
        config->sample_read == NULL)
        return NULL;
    chip = (K054539 *)calloc(1, sizeof(*chip));
    if (chip == NULL)
        return NULL;
    chip->clock_hz = config->clock_hz;
    chip->sample_rate = config->clock_hz / K054539_CLOCK_DIVIDER;
    chip->flags = config->flags | K054539_FLAG_UPDATE_AT_KEYON;
    chip->sample_read = config->sample_read;
    chip->timer_line = config->timer_line;
    chip->analog_pan = config->analog_pan;
    chip->opaque = config->opaque;
    for (i = 0; i < 256; ++i)
        chip->voltab[i] = pow(10.0, (-36.0 * (double)i / 0x40) / 20.0) / 4.0;
    for (i = 0; i < 15; ++i)
        chip->pantab[i] = sqrt((double)i) / sqrt(14.0);
    for (i = 0; i < K054539_VOICES; ++i)
        chip->gain[i] = 1.0;
    return chip;
}

void k054539_destroy(K054539 *chip)
{
    if (chip != NULL && !mutation_allowed(chip))
        return;
    free(chip);
}

uint32_t k054539_sample_rate(const K054539 *chip)
{
    return chip == NULL ? 0 : chip->sample_rate;
}

uint64_t k054539_cycles(const K054539 *chip)
{
    return chip == NULL ? 0 : chip->cycles;
}

int k054539_next_timer_cycle(const K054539 *chip, uint64_t *cycle)
{
    double due;
    if (cycle == NULL)
        return 0;
    *cycle = UINT64_MAX;
    if (chip == NULL || !chip->timer_active)
        return 0;
    due = ceil(chip->timer_next_cycle);
    if (!isfinite(due) || due < 0.0 || due >= K054539_MAX_EXACT_TIMER_CYCLE)
        return 0;
    *cycle = (uint64_t)due;
    return 1;
}

static void timer_event(K054539 *chip)
{
    if (chip->regs[0x22f] & 0x20) {
        chip->timer_state ^= 1;
        timer_line(chip, chip->timer_state);
    }
    chip->timer_next_cycle += chip->timer_period_cycles;
}

int k054539_advance_cycles(K054539 *chip, uint64_t cycles,
                           K054539SampleSink sink, void *sink_opaque)
{
    uint64_t target, next_sample;
    int stream_active;
    if (chip == NULL)
        return -1;
    if (!mutation_allowed(chip))
        return -2;
    if (cycles > UINT64_MAX - chip->cycles)
        return -1;
    target = chip->cycles + cycles;
    if (chip->timer_active && target >= K054539_MAX_EXACT_TIMER_CYCLE)
        return -1;
    /* MAME's stream has no callback in this mode and the disabled device
     * leaves all voice/reverb state untouched, so a timer-only jump can be
     * serviced without iterating silent /384 boundaries. */
    if (sink == NULL && !(chip->regs[0x22f] & 1) && !chip->timer_active) {
        chip->cycles = target;
        return 0;
    }
    stream_active = sink != NULL || (chip->regs[0x22f] & 1);
    while (chip->cycles < target) {
        uint64_t next;
        uint64_t timer_cycle = 0;
        int has_sample = stream_active;
        int has_timer = 0;
        if (has_sample) {
            uint64_t distance = K054539_CLOCK_DIVIDER -
                                (chip->cycles % K054539_CLOCK_DIVIDER);
            if (distance <= UINT64_MAX - chip->cycles)
                next_sample = chip->cycles + distance;
            else
                has_sample = 0;
        }
        if (chip->timer_active) {
            double due = ceil(chip->timer_next_cycle);
            if (due < (double)UINT64_MAX && due >= 0.0) {
                timer_cycle = (uint64_t)due;
                has_timer = 1;
            }
        }
        if (!has_sample && !has_timer) {
            chip->cycles = target;
            break;
        }
        next = !has_sample ? timer_cycle :
               !has_timer ? next_sample :
               next_sample < timer_cycle ? next_sample : timer_cycle;
        if (next > target) {
            chip->cycles = target;
            break;
        }
        chip->cycles = next;
        /* On coincident device events, run the timer before the audio sample. */
        if (has_timer && timer_cycle == next)
            timer_event(chip);
        if (has_sample && next_sample == next)
            render_sample(chip, sink, sink_opaque);
    }
    return 0;
}

static void pointer_advance(K054539 *chip)
{
    chip->cur_ptr = (chip->cur_ptr + 1) & 0x1ffff;
}

int k054539_write(K054539 *chip, uint32_t offset, uint8_t data)
{
    int latch;
    unsigned ch;
    if (chip == NULL || offset >= K054539_REGISTER_BYTES)
        return -1;
    if (!mutation_allowed(chip))
        return -2;
    latch = (chip->flags & K054539_FLAG_UPDATE_AT_KEYON) &&
            (chip->regs[0x22f] & 1);
    if (latch && offset < 0x100) {
        int reg = (int)(offset & 0x1f) - 0x0c;
        if (reg >= 0 && reg <= 2) {
            chip->posreg_latch[offset >> 5][reg] = data;
            return 0;
        }
    } else {
        switch (offset) {
        case 0x13f: {
            int pan = data >= 0x11 && data <= 0x1f ? data - 0x11 : 7;
            if (chip->analog_pan != NULL) {
                ++chip->callback_depth;
                chip->analog_pan(chip->opaque, chip->pantab[pan],
                                 chip->pantab[0xe - pan]);
                --chip->callback_depth;
            }
            break;
        }
        case 0x214:
            if (latch) {
                for (ch = 0; ch < K054539_VOICES; ++ch) {
                    if (data & (1u << ch)) {
                        uint8_t *position = chip->regs + (ch << 5) + 0x0c;
                        memcpy(position, chip->posreg_latch[ch], 3);
                        keyon(chip, ch);
                    }
                }
            } else {
                for (ch = 0; ch < K054539_VOICES; ++ch)
                    if (data & (1u << ch))
                        keyon(chip, ch);
            }
            break;
        case 0x215:
            for (ch = 0; ch < K054539_VOICES; ++ch)
                if (data & (1u << ch))
                    keyoff(chip, ch);
            break;
        case 0x227: {
            float base_hz = ((float)chip->clock_hz / 384.0f) / 14400.0f;
            double period_seconds;
            float frequency = (float)(38 + data) * base_hz;
            double period_cycles;
            double next_cycle;
            if (chip->cycles > K054539_MAX_EXACT_TIMER_CYCLE ||
                frequency <= 0.0f)
                return -1;
            period_seconds = 1.0 / (double)frequency;
            period_seconds /= 2.0;
            period_cycles = period_seconds * chip->clock_hz;
            next_cycle = (double)chip->cycles + period_cycles;
            if (!isfinite(period_cycles) || !isfinite(next_cycle) ||
                next_cycle >= (double)K054539_MAX_EXACT_TIMER_CYCLE)
                return -1;
            chip->timer_frequency = frequency;
            chip->timer_period_cycles = period_cycles;
            chip->timer_next_cycle = next_cycle;
            chip->timer_active = 1;
            chip->timer_state = 0;
            timer_line(chip, 0);
            break;
        }
        case 0x22d:
            if (chip->rom_addr == 0x80) {
                uint32_t address = (chip->cur_ptr & 0x3fff) |
                                   ((chip->cur_ptr & 0x10000) >> 2);
                chip->reverb_ram.bytes[address] = data;
            }
            pointer_advance(chip);
            break;
        case 0x22e:
            chip->rom_addr = data;
            chip->cur_ptr = 0;
            break;
        case 0x22f:
            if (!(data & 0x20)) {
                chip->timer_state = 0;
                timer_line(chip, 0);
            }
            break;
        default:
            break;
        }
    }
    chip->regs[offset] = data;
    return 0;
}

uint8_t k054539_read(K054539 *chip, uint32_t offset,
                     int side_effects_disabled)
{
    if (chip == NULL || offset >= K054539_REGISTER_BYTES)
        return 0xff;
    if (offset == 0x22d) {
        if (chip->callback_depth != 0) {
            chip->callback_fault = 1;
            return 0xff;
        }
        if (chip->regs[0x22f] & 0x10) {
            uint8_t value;
            if (chip->rom_addr == 0x80) {
                uint32_t address = (chip->cur_ptr & 0x3fff) |
                                   ((chip->cur_ptr & 0x10000) >> 2);
                value = chip->reverb_ram.bytes[address];
            } else {
                uint32_t address = (0x20000u * chip->rom_addr) + chip->cur_ptr;
                value = sample_byte(chip, address);
            }
            if (!side_effects_disabled)
                pointer_advance(chip);
            return value;
        }
        return 0;
    }
    return chip->regs[offset];
}

void k054539_set_flags(K054539 *chip, int flags)
{
    if (chip != NULL && mutation_allowed(chip))
        chip->flags = flags;
}

int k054539_set_gain(K054539 *chip, unsigned channel, double gain)
{
    if (chip == NULL || channel >= K054539_VOICES)
        return -1;
    if (!mutation_allowed(chip))
        return -2;
    if (!isfinite(gain))
        return -1;
    if (gain >= 0.0)
        chip->gain[channel] = gain;
    return 0;
}

void k054539_reset(K054539 *chip)
{
    if (chip == NULL)
        return;
    if (!mutation_allowed(chip))
        return;
    chip->regs[0x22c] = 0;
    chip->regs[0x22f] = 0;
    memset(&chip->reverb_ram, 0, sizeof(chip->reverb_ram));
    chip->timer_active = 0;
}

int k054539_in_callback(const K054539 *chip)
{
    return chip != NULL && chip->callback_depth != 0;
}

int k054539_callback_fault(const K054539 *chip)
{
    return chip != NULL && chip->callback_fault;
}
