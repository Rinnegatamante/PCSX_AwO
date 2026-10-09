// license:BSD-3-Clause
// copyright-holders:R. Belmont, superctr
#include "../libpcsxcore/arcade/c352.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    BUSY = 0x8000, KEYON = 0x4000, KEYOFF = 0x2000,
    LOOPHIST = 0x0800, MULAW = 0x0008, FILTER = 0x0004,
    LOOP = 0x0002
};

static void reg(C352 *c, unsigned voice, unsigned index, uint16_t value)
{
    c352_write(c, voice * 8 + index, value, 0xffff);
}

static void keyon(C352 *c)
{
    c352_write(c, 0x202, 0, 0xffff);
}

static void test_bus_and_pcm(void)
{
    uint8_t rom[64];
    int16_t out[600 * 4];
    C352 *c;
    size_t i;
    memset(rom, 0x40, sizeof(rom));
    c = c352_create(rom, sizeof(rom), 25401600, 288);
    assert(c && c352_sample_rate(c) == 88200);

    reg(c, 0, 0, 0x8080);          /* front L/R target volumes */
    reg(c, 0, 1, 0x4040);          /* rear L/R target volumes */
    reg(c, 0, 2, 0x8000);          /* half sample per output frame */
    reg(c, 0, 3, KEYON | FILTER | LOOP);
    reg(c, 0, 4, 0); reg(c, 0, 5, 0); reg(c, 0, 6, 63);
    assert(c352_read(c, 0) == 0x8080);
    assert(c352_read(c, 1) == 0x4040);
    assert(c352_read(c, 0x200) == 0);
    c352_write(c, 0, 0x1234, 0x00ff); /* write only low byte */
    assert(c352_read(c, 0) == 0x8034);
    reg(c, 0, 0, 0x8080);
    keyon(c);
    assert(c352_read(c, 3) == (BUSY | FILTER | LOOP));
    c352_generate(c, out, 600);
    for (i = 0; i < 4; ++i) assert(out[i] == 8); /* first one-step volume ramp */
    assert(out[599 * 4] > 0);
    assert(out[599 * 4 + 1] == out[599 * 4]);
    assert(out[599 * 4 + 2] > 0 && out[599 * 4 + 2] < out[599 * 4]);
    assert(out[599 * 4 + 3] == out[599 * 4 + 2]);
    c352_destroy(c);
}

static void test_mulaw_and_loop(void)
{
    uint8_t rom[64];
    int16_t pcm[4 * 300];
    C352 *c;
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xff;                /* negative mu-law end of range */
    rom[1] = 0x7f;
    c = c352_create(rom, sizeof(rom), 25401600, 288);
    assert(c);
    reg(c, 0, 0, 0x8080);
    reg(c, 0, 2, 0xffff);
    reg(c, 0, 3, KEYON | MULAW | FILTER | LOOP);
    reg(c, 0, 5, 0); reg(c, 0, 6, 1); reg(c, 0, 7, 0);
    keyon(c);
    c352_generate(c, pcm, 300);
    assert(c352_read(c, 3) & BUSY);
    assert(c352_read(c, 3) & LOOPHIST);
    assert(pcm[0] < 0);          /* 0xff is the negative mu-law code */
    c352_destroy(c);
}

static void test_one_shot_and_reset(void)
{
    uint8_t rom[8];
    int16_t pcm[16 * 4];
    C352 *c;
    memset(rom, 0x7f, sizeof(rom));
    c = c352_create(rom, sizeof(rom), 25401600, 288);
    assert(c);
    reg(c, 0, 0, 0xffff);
    reg(c, 0, 2, 0xffff);
    reg(c, 0, 3, KEYON | FILTER);
    reg(c, 0, 6, 0);
    keyon(c);
    c352_generate(c, pcm, 4);
    assert(!(c352_read(c, 3) & BUSY));
    assert(c352_read(c, 3) & KEYOFF);
    c352_write(c, 0x202, 0, 0xffff);
    assert(!(c352_read(c, 3) & KEYOFF));
    c352_reset(c);
    assert(c352_read(c, 3) == 0);
    assert(c352_read(c, 0x200) == 0);
    c352_destroy(c);
}

/* Interpolating between full-scale samples must stay between them. MAME
 * multiplies counter * step in u32 and keeps the result in s16; a signed
 * product overflowed for large steps and shifted the sample by 65536. */
static void test_interpolation_full_swing(void)
{
    uint8_t rom[64];
    int16_t pcm[2000 * 4];
    const int limit = (32768 * 0xff >> 8) >> 3;
    C352 *c;
    size_t i;
    for (i = 0; i < sizeof(rom); ++i)
        rom[i] = i & 1 ? 0x7f : 0x80; /* +32512, -32768 */
    c = c352_create(rom, sizeof(rom), 25401600, 288);
    assert(c);
    reg(c, 0, 0, 0xffff);
    reg(c, 0, 2, 0x1000);
    reg(c, 0, 3, KEYON | LOOP);
    reg(c, 0, 5, 0); reg(c, 0, 6, sizeof(rom) - 1); reg(c, 0, 7, 0);
    keyon(c);
    c352_generate(c, pcm, 2000);
    for (i = 0; i < 2000; ++i) {
        assert(pcm[i * 4] >= -limit && pcm[i * 4] <= limit);
        assert(pcm[i * 4 + 1] == pcm[i * 4]);
    }
    c352_destroy(c);
}

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* c352_generate mixes one voice over a whole block (calls of 2 frames or
 * more without noise) and MAME's way, every voice per frame, otherwise.
 * Random voice programs on two chips: one generated a frame per call, the
 * other in blocks; outputs and readable registers must stay equal. */
static void test_block_order_equivalence(void)
{
    static uint8_t rom[0x20000];
    static int16_t a[300 * 4], b[300 * 4];
    C352 *ca, *cb;
    unsigned round, i;
    for (i = 0; i < sizeof(rom); ++i)
        rom[i] = (uint8_t)rng();
    ca = c352_create(rom, sizeof(rom), 25401600, 288);
    cb = c352_create(rom, sizeof(rom), 25401600, 288);
    assert(ca && cb);
    for (round = 0; round < 4000; ++round) {
        unsigned writes = rng() % 6, frames = 1 + rng() % 300, w;
        for (w = 0; w < writes; ++w) {
            unsigned voice = rng() % 32, index = rng() % 8;
            uint16_t value = (uint16_t)rng();
            if (index == 3) {
                /* flags: mostly PCM, sometimes noise, key on or off */
                value &= 0x07ff;
                if (rng() % 8) value &= (uint16_t)~0x0010;
                if (rng() % 2) value |= KEYON;
                else if (rng() % 3 == 0) value |= KEYOFF;
            } else if (index == 4) {
                value &= 1;                    /* bank inside the ROM */
            } else if (index == 2) {
                value = (uint16_t)(rng() % 3 == 0 ? value : value & 0x3fff);
            }
            reg(ca, voice, index, value);
            reg(cb, voice, index, value);
        }
        if (rng() % 2) {
            keyon(ca);
            keyon(cb);
        }
        for (i = 0; i < frames; ++i)
            c352_generate(ca, a + i * 4, 1);
        c352_generate(cb, b, frames);
        assert(memcmp(a, b, frames * 4 * sizeof(a[0])) == 0);
        for (i = 0; i < 0x100; ++i)
            assert(c352_read(ca, i) == c352_read(cb, i));
    }
    c352_destroy(ca);
    c352_destroy(cb);
}

int main(void)
{
    test_bus_and_pcm();
    test_mulaw_and_loop();
    test_one_shot_and_reset();
    test_interpolation_full_swing();
    test_block_order_equivalence();
    puts("C352 synthetic tests passed");
    return 0;
}
