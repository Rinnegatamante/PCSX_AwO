/*
 * GPU profiler captures (psp2-cex-gpues4-prof) for the vitaGL renderer, built
 * only with VITA_GPUPROF=1 (Makefile.vita).
 *
 * Trigger ux0:data/pcsx-rearmed/gpuprof = "start,stop[,events]" renderer
 * swaps (empty: 600,660), one capture per launch: records that window into
 * ux0:data/pcsx-rearmed/gpuprof.csv for psp2-cex-gpues4-prof's
 * tools/analyze.py. The kernel plugin psp2_gpuprof.skprx must be loaded before
 * the app starts (the gpuprof loader homebrew); the eboot imports it.
 *
 * Per-draw GPU timing needs the GXM frame/scene counters at the start of the
 * capture: an observer on every GXM call counts presents and scenes from GXM's
 * first call, so the capture is seeded with the exact values at a present.
 */
#ifndef VITA_GPUPROF_H
#define VITA_GPUPROF_H

#ifdef VITA_GPUPROF
/* Before vglInitExtended: the observer must see GXM from its first call. */
void vita_gpuprof_install(void);
/* Reads the trigger and allocates the event buffer. */
void vita_gpuprof_session_begin(const char *id);
/* On the renderer thread after each present; `frame` = presents so far. */
void vita_gpuprof_after_frame(unsigned frame);
/* Writes a capture still running. */
void vita_gpuprof_session_end(void);
/* The renderer's labels (pcsx_vita_gpu_label_cb): 0 open a pass, 1 close it,
 * 2 name the following draws (NULL clears). */
void vita_gpuprof_label(int kind, const char *name, unsigned handle);
#else
static inline void vita_gpuprof_install(void) {}
static inline void vita_gpuprof_session_begin(const char *id) { (void)id; }
static inline void vita_gpuprof_after_frame(unsigned frame) { (void)frame; }
static inline void vita_gpuprof_session_end(void) {}
#endif

#endif
