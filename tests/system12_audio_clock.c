#include "../libpcsxcore/system12_audio_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t reference(uint64_t cycle)
{
    /* Independent wide-integer version of MAME's accumulated local_time and
     * inclusive stream sample index, not the core's incremental conversion. */
    __uint128_t time = (__uint128_t)cycle * (UINT64_C(1000000000000000000) / 16934400);
    return cycle ? (uint64_t)(time * 88200 / UINT64_C(1000000000000000000)) + 1 : 0;
}

int main(void)
{
    System12AudioClock clock = {0}, whole = {0};
    uint64_t target = 0, second = 0, cycle = 0;
    /* Includes both sides of sample edges and exact clock-second boundaries. */
    for (cycle = 0; cycle <= 100000; cycle++) {
        assert(system12_audio_clock_advance(&clock, cycle, &target) == 0);
        assert(target == reference(cycle));
    }
    for (unsigned hour = 1; hour <= 24; hour++) {
        uint64_t edge = UINT64_C(16934400) * 3600 * hour;
        for (unsigned delta = 0; delta <= 3; delta++) {
            cycle = edge + delta;
            assert(system12_audio_clock_advance(&clock, cycle, &target) == 0);
            assert(target == reference(cycle));
        }
    }
    assert(system12_audio_clock_advance(&whole, cycle, &second) == 0);
    assert(second == target && whole.samples == clock.samples && whole.phase == clock.phase);
    System12AudioClock before = clock;
    second = UINT64_MAX;
    assert(system12_audio_clock_advance(&clock, cycle - 1, &second) == -1);
    assert(memcmp(&clock, &before, sizeof(clock)) == 0 && second == UINT64_MAX);
    memset(&clock, 0, sizeof(clock));
    assert(system12_audio_clock_advance(&clock, 0, &target) == 0 && target == 0);
    uint32_t random = 0x3523002;
    cycle = 0;
    for (unsigned i = 0; i < 10000; i++) {
        random = random * 1664525u + 1013904223u;
        cycle += random % 1200001u;
        assert(system12_audio_clock_advance(&clock, cycle, &target) == 0);
        assert(target == reference(cycle));
    }
    puts("System12 C352 clock: MAME phase, split/chunk, 24-hour and reset tests PASS");
    return 0;
}
