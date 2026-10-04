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

int main(void)
{
    test_bus_and_pcm();
    test_mulaw_and_loop();
    test_one_shot_and_reset();
    puts("C352 synthetic tests passed");
    return 0;
}
