/* GPL-2.0-or-later. Opt-in diagnostics; never advances an emulated clock. */
#ifndef PCSX_SYSTEM12_PROFILE_H
#define PCSX_SYSTEM12_PROFILE_H
#include <stdint.h>
enum pcsx_profile_section {
    PCSX_PROF_CPU, PCSX_PROF_H8, PCSX_PROF_PCM, PCSX_PROF_BOARD_AUDIO,
    PCSX_PROF_GPU_LIST, PCSX_PROF_GPU_FLUSH, PCSX_PROF_VIDEO,
    PCSX_PROF_PRESENT_COPY, PCSX_PROF_PRESENT_WAIT, PCSX_PROF_AUDIO_WAIT,
    PCSX_PROF_COUNT
};
#ifdef PCSX_SYSTEM12_PROFILE
#ifdef VITA
/* Optional sink for PERF lines (the standalone frontend tees them to a file). */
extern void (*pcsx_profile_log)(const char *text);
#endif
void pcsx_profile_reset(void);
uint64_t pcsx_profile_begin(enum pcsx_profile_section section, unsigned stride);
void pcsx_profile_end(enum pcsx_profile_section section, uint64_t started);
void pcsx_profile_frame_begin(uint32_t cycle, uint64_t h8, uint64_t pcm);
void pcsx_profile_frame_end(uint32_t cycle, uint64_t h8, uint64_t pcm,
                            uint32_t clock_hz, uint32_t gpu_status);
#else
static inline void pcsx_profile_reset(void) { }
static inline uint64_t pcsx_profile_begin(enum pcsx_profile_section s, unsigned n)
{ (void)s; (void)n; return 0; }
static inline void pcsx_profile_end(enum pcsx_profile_section s, uint64_t t)
{ (void)s; (void)t; }
#endif
#endif
