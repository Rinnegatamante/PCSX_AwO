/* System12 C352 stream phase in the H8 clock domain. */
#ifndef PCSX_SYSTEM12_AUDIO_CLOCK_H
#define PCSX_SYSTEM12_AUDIO_CLOCK_H
#include <stdint.h>

typedef struct {
    uint64_t cycle, samples, phase;
} System12AudioClock;

/* MAME's accumulated H8 period is floor(1e18 / 16934400) attoseconds.
 * At 88200 Hz, reduced samples/cycle = 26041666666617 / 5000000000000000.
 * The stream includes the current sample, except at time zero. Reset this
 * cursor with its CPU epoch. A backwards timestamp is rejected unchanged. */
static inline int system12_audio_clock_advance(System12AudioClock *clock,
    uint64_t cycle, uint64_t *target)
{
    const uint64_t step = UINT64_C(26041666666617);
    const uint64_t denominator = UINT64_C(5000000000000000);
    uint64_t elapsed;
    if (cycle < clock->cycle) return -1;
    elapsed = cycle - clock->cycle;
    clock->cycle = cycle;
    while (elapsed) {
        /* 600000 * step + (denominator - 1) fits uint64_t. */
        uint64_t chunk = elapsed > 600000 ? 600000 : elapsed;
        clock->phase += chunk * step;
        clock->samples += clock->phase / denominator;
        clock->phase %= denominator;
        elapsed -= chunk;
    }
    *target = clock->samples + (cycle != 0);
    return 0;
}
#endif
