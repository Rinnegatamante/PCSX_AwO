/* Source-derived C352 stream update boundary test.
 * MAME's sound_stream::update renders get_current_sample_index() inclusive.
 * H8 local_time and the scheduler accumulate elapsed H8 cycles using
 * floor(1e18 / H8_HZ) attoseconds per cycle. The stream renders the current
 * sample index inclusively. The integer model below reproduces that sequence.
 */
#include "../libpcsxcore/arcade/c352.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { H8_HZ = 16934400, SAMPLE_HZ = 88200, H8_PER_SAMPLE = 192 };
static const uint64_t ATTOSECONDS = UINT64_C(1000000000000000000);

typedef struct {
    C352 *chip;
    uint64_t frames;
    int16_t audio[32 * 4];
} Stream;

/* Model MAME's local_time() -> machine.time() -> stream::update() sample
 * cursor for the H8 callback timestamps used by this board. */
static uint64_t mame_frame_count(uint64_t h8_cycles)
{
    uint64_t attoseconds_per_tick = ATTOSECONDS / H8_HZ;
    if (h8_cycles == 0) return 0; /* sound_stream::update skips time zero */
    /* MAME's scheduler adds each executed H8 slice to its accumulated base;
     * both use this same truncated per-cycle attosecond value. `local_time()`
     * adds the current slice before sound_stream::update(). Use a wide
     * intermediate so the accumulated timeline and sample-index multiply do
     * not overflow for the test's long-range clock checks. */
    __uint128_t now_attoseconds = (__uint128_t)h8_cycles * attoseconds_per_tick;
    uint64_t current_index = (uint64_t)(now_attoseconds * SAMPLE_HZ / ATTOSECONDS);
    return current_index + 1;
}

static void update_at(Stream *s, uint64_t h8_cycles)
{
    uint64_t target = mame_frame_count(h8_cycles);
    assert(target >= s->frames && target <= 32);
    size_t count = (size_t)(target - s->frames);
    if (count) c352_generate(s->chip, &s->audio[s->frames * 4], count);
    s->frames = target;
}

static void write_reg(C352 *c, unsigned reg, uint16_t value)
{
    c352_write(c, reg, value, 0xffff);
}

int main(void)
{
    uint8_t rom[64];
    Stream s;
    memset(rom, 0x40, sizeof(rom));
    memset(&s, 0, sizeof(s));
    s.chip = c352_create(rom, sizeof(rom), 25401600, 288);
    assert(s.chip && c352_sample_rate(s.chip) == SAMPLE_HZ);

    /* A register access exactly on output edge 8 updates samples 0..7 first;
     * key-on therefore first contributes to output sample 8. */
    assert(mame_frame_count(8 * H8_PER_SAMPLE) == 8);
    for (uint64_t cycle = 0; cycle <= 100000; ++cycle) {
        uint64_t expected = cycle ? (cycle + H8_PER_SAMPLE - 1) / H8_PER_SAMPLE : 0;
        assert(mame_frame_count(cycle) == expected);
    }
    const unsigned timeline_seconds[] = {1, 60, 3600};
    for (unsigned t = 0; t < sizeof(timeline_seconds) / sizeof(timeline_seconds[0]); ++t) {
        uint64_t seconds = timeline_seconds[t];
        uint64_t edge = seconds * H8_HZ;
        assert(mame_frame_count(edge) == edge / H8_PER_SAMPLE);
        assert(mame_frame_count(edge + 1) == edge / H8_PER_SAMPLE + 1);
    }
    update_at(&s, 8 * H8_PER_SAMPLE);
    write_reg(s.chip, 0, 0xffff);
    write_reg(s.chip, 2, 0xffff);
    write_reg(s.chip, 3, 0x4000 | 0x0004 | 0x0002); /* key-on/filter/loop */
    write_reg(s.chip, 5, 0);
    write_reg(s.chip, 6, 63);
    write_reg(s.chip, 0x202, 0);
    update_at(&s, 9 * H8_PER_SAMPLE);
    assert(s.frames == 9);
    assert(s.audio[7 * 4] == 0);
    assert(s.audio[8 * 4] == 8);

    /* One H8 cycle after edge 12, stream::update has also emitted frame 12;
     * key-off begins at frame 13, preserving the already-rendered frame 12. */
    assert(mame_frame_count(12 * H8_PER_SAMPLE) == 12);
    assert(mame_frame_count(12 * H8_PER_SAMPLE + 1) == 13);
    update_at(&s, 12 * H8_PER_SAMPLE);
    assert(s.audio[11 * 4] != 0);
    update_at(&s, 12 * H8_PER_SAMPLE + 1);
    uint16_t flags = c352_read(s.chip, 3);
    write_reg(s.chip, 3, flags | 0x2000); /* key-off request */
    write_reg(s.chip, 0x202, 0);          /* execute key-off */
    update_at(&s, 13 * H8_PER_SAMPLE + 1);
    assert(s.audio[12 * 4] != 0);
    assert(s.audio[13 * 4] == 0);

    c352_destroy(s.chip);
    puts("C352 MAME stream timing test passed");
    return 0;
}
