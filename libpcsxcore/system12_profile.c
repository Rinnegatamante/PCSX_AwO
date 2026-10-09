/* GPL-2.0-or-later. Diagnostic scopes; GPU and board threads may update them. */
#include "system12_profile.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#ifdef VITA
#include <stdarg.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
void (*pcsx_profile_log)(const char *text);
static void profile_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (pcsx_profile_log) pcsx_profile_log(buf);
    else sceClibPrintf("%s", buf);
}
#define PROFILE_PRINTF profile_printf
#else
#include <time.h>
#define PROFILE_PRINTF printf
#endif

/* Board-thread scopes (H8, C352) update buckets while the main thread prints
 * them: counters are relaxed atomics. */
#define PLOAD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define PSTORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define PADD(x, v) __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)

struct profile_bucket {
    uint64_t calls, samples, sample_us;
    unsigned stride;
};
static struct profile_bucket buckets[PCSX_PROF_COUNT];
static uint64_t wall_started, psx_cycles, h8_cycles, pcm_samples;
static uint64_t h8_before, pcm_before;
static uint32_t cycle_before, frames, modes[4], epoch;
static int enabled;

static void clear_buckets(void)
{
    unsigned i;
    for (i = 0; i < PCSX_PROF_COUNT; ++i) {
        PSTORE(buckets[i].calls, 0);
        PSTORE(buckets[i].samples, 0);
        PSTORE(buckets[i].sample_us, 0);
        PSTORE(buckets[i].stride, 0);
    }
}

void pcsx_profile_reset(void)
{
    if (PLOAD(enabled)) epoch++;
    PSTORE(enabled, 0);
    psx_cycles = h8_cycles = pcm_samples = 0;
    frames = 0;
    clear_buckets();
    memset(modes, 0, sizeof(modes));
}

static uint64_t now_us(void)
{
#ifdef VITA
    return sceKernelGetProcessTimeWide();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
#endif
}

uint64_t pcsx_profile_begin(enum pcsx_profile_section section, unsigned stride)
{
    struct profile_bucket *b;
    if (!PLOAD(enabled) || (unsigned)section >= PCSX_PROF_COUNT || !stride) return 0;
    b = &buckets[section];
    PSTORE(b->stride, stride);
    if ((PADD(b->calls, 1) % stride) != 0) return 0;
    PADD(b->samples, 1);
    return now_us() + 1; /* zero denotes an unsampled scope. */
}

void pcsx_profile_end(enum pcsx_profile_section section, uint64_t started)
{
    struct profile_bucket *b;
    uint64_t elapsed;
    if (!PLOAD(enabled) || !started || (unsigned)section >= PCSX_PROF_COUNT) return;
    b = &buckets[section];
    elapsed = now_us() - (started - 1);
    PADD(b->sample_us, elapsed);
}

void pcsx_profile_frame_begin(uint32_t cycle, uint64_t h8, uint64_t pcm)
{
    if (!PLOAD(enabled)) {
        clear_buckets();
        wall_started = now_us();
        PSTORE(enabled, 1);
    }
    cycle_before = cycle;
    h8_before = h8;
    pcm_before = pcm;
}

void pcsx_profile_frame_end(uint32_t cycle, uint64_t h8, uint64_t pcm,
                            uint32_t clock_hz, uint32_t gpu_status)
{
    static const char *names[] = {"cpu_inclusive", "h8", "c352",
        "board_audio", "gpu_list", "gpu_flush", "video",
        "present_copy", "present_wait", "audio_wait"};
    unsigned i, mode = ((gpu_status >> 19) & 1) | (((gpu_status >> 20) & 1) << 1);
    if (!PLOAD(enabled)) return;
    psx_cycles += (uint32_t)(cycle - cycle_before); /* include u32 wrap */
    h8_cycles += h8 - h8_before;
    pcm_samples += pcm - pcm_before;
    modes[mode]++;
    if (++frames % 600) return;
    PROFILE_PRINTF("pcsx-vita: PERF frames=%u wall_us=%" PRIu64
           " psx_cycles=%" PRIu64 " main_cycle_hz=%u h8_cycles=%" PRIu64
           " h8_hz=16934400 pcm_samples=%" PRIu64
           " pcm_hz=88200 modes=%u,%u,%u,%u epoch=%u\n", frames,
           now_us() - wall_started, psx_cycles, clock_hz, h8_cycles,
           pcm_samples, modes[0], modes[1], modes[2], modes[3], epoch);
    for (i = 0; i < PCSX_PROF_COUNT; ++i) {
        struct profile_bucket *b = &buckets[i];
        uint64_t calls = PLOAD(b->calls), samples = PLOAD(b->samples);
        uint64_t sample_us = PLOAD(b->sample_us);
        PROFILE_PRINTF("pcsx-vita: PERF section=%s calls=%" PRIu64
               " samples=%" PRIu64 " stride=%u sample_us=%" PRIu64
               " estimated_us=%" PRIu64 " frames=%u\n", names[i], calls,
               samples, PLOAD(b->stride), sample_us, samples ? sample_us * calls / samples : 0, frames);
    }
#ifndef VITA
    fflush(stdout);
#endif
}
