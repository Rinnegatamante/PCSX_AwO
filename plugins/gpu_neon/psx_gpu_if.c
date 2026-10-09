/*
 * (C) Gražvydas "notaz" Ignotas, 2011
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <assert.h>
#include "../../libpcsxcore/system12_profile.h"
#include "../../include/compiler_features.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

extern const unsigned char cmd_lengths[256];
#define command_lengths cmd_lengths

static int initialized;

#define PCSX

static noinline void sync_enhancement_buffers(int x, int y, int w, int h);

#include "../gpulib/gpu.h"
#include "psx_gpu/psx_gpu.c"
#include "psx_gpu/psx_gpu_parse.c"

static psx_gpu_struct egpu __attribute__((aligned(256)));

#ifdef PSX_GPU_PIPELINE
#include <stddef.h>
#include <string.h>
#include "../../frontend/pcsxr-threads.h"

/* Two-stage renderer. The GPU thread parses commands and sets up triangles,
 * spans and blocks (stage 1); each full block buffer goes through a ring to
 * a second thread that textures, shades and blends it into VRAM (stage 2),
 * in submission order. A ring slot is a copy of psx_gpu_struct up to the
 * end of blocks[]: the block handlers read nothing past it, so stage 2 runs
 * them on the slot itself, with the state its buffer was set up with (the
 * renderer flushes before every state change those handlers depend on).
 *
 * Only stage 1 touches the texture caches and their dirty masks: before a
 * buffer is queued it rebuilds the 4bpp/8bpp cache its handler reads, as
 * texture_blocks would at that point. Stage 1 waits for stage 2 to finish
 * whenever it is about to read or write VRAM that queued buffers may still
 * write (their viewports' pages): such a cache rebuild, a textured sprite
 * (its setup reads texels and the CLUT), a fill, and every access from
 * gpulib, which first calls renderer_flush_queues. So every VRAM read and
 * write happens in the same order as in the single-threaded renderer. */
#define PIPELINE_SLOTS 16
#define PIPELINE_SLOT_BYTES \
  ((offsetof(psx_gpu_struct, span_uvrg_offset) + 255) & ~(size_t)255)
#ifndef PIPELINE_SPIN
#define PIPELINE_SPIN 2000
#endif

int psx_gpu_pipeline_requested;

static struct {
  u8 *slots;
  void *slots_alloc;
  unsigned head;      /* buffers queued (stage 1 writes) */
  unsigned tail;      /* buffers done (stage 2 writes) */
  int waiting_producer, waiting_consumer, quit;
  /* Stage 1 only: bounding box of the viewports of queued buffers, the
   * only VRAM they write (inclusive; any = 0 when nothing is queued). */
  int pend_any, pend_x0, pend_y0, pend_x1, pend_y1;
  slock_t *lock;
  scond_t *work, *done;
  sthread_t *thread;
  int active;
} pl;

#ifdef PSX_GPU_PIPELINE_STATS
/* Why stage 1 waited for stage 2: tools/gpu-replay builds print these. */
static unsigned pl_stats[PLS_COUNT], pl_stats_waited[PLS_COUNT];
static unsigned long long pl_stats_blocks;
static int pl_reason = PLS_DIRECT;
static void pipeline_stat(int reason)
{
  pl_stats[reason]++;
  pl_reason = reason;
}
void psx_gpu_pipeline_print_stats(void)
{
  static const char *names[PLS_COUNT] = { "submit", "ring-full", "cache", "sprite", "direct", "flush" };
  int i;
  for (i = 0; i < PLS_COUNT; i++)
    SysPrintf("gpu_neon pipeline: %-9s %7u calls, %7u waited\n", names[i], pl_stats[i], pl_stats_waited[i]);
  SysPrintf("gpu_neon pipeline: %llu blocks queued\n", pl_stats_blocks);
}
#endif
#define pl_load(v) __atomic_load_n(&(v), __ATOMIC_SEQ_CST)
#define pl_store(v, x) __atomic_store_n(&(v), (x), __ATOMIC_SEQ_CST)

static void pipeline_run_slot(psx_gpu_struct *g)
{
  render_block_handler_struct *h = g->render_block_handler;
  int i;
  /* Shading and blending read each block's framebuffer pixels; setup's
   * prefetch of them ran on stage 1's core. */
  for (i = 0; i < g->num_blocks; i++)
    __builtin_prefetch(g->blocks[i].fb_ptr, 1);
#if defined(__arm__) && defined(NEON_BUILD) && !defined(SIMD_BUILD)
  __asm__ __volatile__("":::"q4","q5","q6","q7");
#endif
  h->texture_blocks(g);
  h->shade_blocks(g);
  h->blend_blocks(g);
#if defined(__arm__) && defined(NEON_BUILD) && !defined(SIMD_BUILD)
  __asm__ __volatile__("":::"q4","q5","q6","q7");
#endif
}

static STRHEAD_RET_TYPE pipeline_thread(void *unused)
{
  unsigned tail = 0, head;
  int i;
  (void)unused;
  for (;;) {
    head = pl_load(pl.head);
    for (i = 0; i < PIPELINE_SPIN && head == tail; i++)
      head = pl_load(pl.head);
    if (head == tail) {
      slock_lock(pl.lock);
      pl_store(pl.waiting_consumer, 1);
      while ((head = pl_load(pl.head)) == tail && !pl.quit)
        scond_wait(pl.work, pl.lock);
      pl_store(pl.waiting_consumer, 0);
      slock_unlock(pl.lock);
      if (head == tail)
        break;   /* quit with nothing queued */
    }
    pipeline_run_slot((psx_gpu_struct *)
      (pl.slots + (tail % PIPELINE_SLOTS) * PIPELINE_SLOT_BYTES));
    pl_store(pl.tail, ++tail);
    if (pl_load(pl.waiting_producer)) {
      slock_lock(pl.lock);
      scond_signal(pl.done);
      slock_unlock(pl.lock);
    }
  }
  STRHEAD_RETURN();
}

/* Stage 1: wait until stage 2 has finished buffer number target - 1. */
static void pipeline_wait_done(unsigned target)
{
  int i;
  if ((int)(pl_load(pl.tail) - target) >= 0)
    return;
#ifdef PSX_GPU_PIPELINE_STATS
  pl_stats_waited[pl_reason]++;
#endif
  for (i = 0; i < PIPELINE_SPIN; i++)
    if ((int)(pl_load(pl.tail) - target) >= 0)
      return;
  slock_lock(pl.lock);
  pl_store(pl.waiting_producer, 1);
  while ((int)(pl_load(pl.tail) - target) < 0)
    scond_wait(pl.done, pl.lock);
  pl_store(pl.waiting_producer, 0);
  slock_unlock(pl.lock);
}


static void pipeline_wait_idle(psx_gpu_struct *psx_gpu)
{
  (void)psx_gpu;
  if (pl.active && pl.head != pl_load(pl.tail))
    pipeline_wait_done(pl.head);
  pl.pend_any = 0;
}

/* Whether VRAM columns x .. x + w - 1 (wrapping at 1024), rows y0 .. y1 may
 * still be written by queued buffers. */
static int pipeline_pending(u32 x, u32 w, u32 y0, u32 y1)
{
  u32 x1;
  if (!pl.pend_any || (s32)y1 < pl.pend_y0 || (s32)y0 > pl.pend_y1)
    return 0;
  x &= 1023;
  if (w >= 1024)
    return 1;
  x1 = x + w - 1;
  if (x1 > 1023)   /* wraps: columns x .. 1023 and 0 .. x1 - 1024 */
    return (s32)x <= pl.pend_x1 || (s32)(x1 - 1024) >= pl.pend_x0;
  return (s32)x <= pl.pend_x1 && (s32)x1 >= pl.pend_x0;
}

/* Whether the texture page of the current state (as many columns as its
 * depth reads) may still be written by queued buffers. */
static int pipeline_texture_pending(psx_gpu_struct *psx_gpu, u32 mode)
{
  u32 page = psx_gpu->current_texture_page;
  u32 y = (page >> 4) * 256;
  return pipeline_pending((page & 15) * 64, 64 << (mode > 2 ? 2 : mode), y, y + 255);
}

static void pipeline_update_cache(psx_gpu_struct *psx_gpu, int depth)
{
  PL_STAT(PLS_CACHE);
  if (pipeline_texture_pending(psx_gpu, depth == 4 ? TEXTURE_MODE_4BPP : TEXTURE_MODE_8BPP))
    pipeline_wait_idle(psx_gpu);
#if defined(__arm__) && defined(NEON_BUILD) && !defined(SIMD_BUILD)
  __asm__ __volatile__("":::"q4","q5","q6","q7");
#endif
  if (depth == 4)
    update_texture_4bpp_cache(psx_gpu);
  else
    update_texture_8bpp_cache(psx_gpu);
#if defined(__arm__) && defined(NEON_BUILD) && !defined(SIMD_BUILD)
  __asm__ __volatile__("":::"q4","q5","q6","q7");
#endif
}

static int pipeline_submit(psx_gpu_struct *psx_gpu)
{
  texture_blocks_function_type *texture_blocks;
  unsigned head;
  u8 *slot;

  if (!pl.active || psx_gpu != &egpu)
    return 0;
  if (gpu.state.enhancement_active) {
    pipeline_wait_idle(psx_gpu);
    return 0;
  }
  texture_blocks = psx_gpu->render_block_handler->texture_blocks;
  if (texture_blocks == texture_blocks_4bpp &&
      (psx_gpu->current_texture_mask & psx_gpu->dirty_textures_4bpp_mask))
    pipeline_update_cache(psx_gpu, 4);
  else if (texture_blocks == texture_blocks_8bpp &&
      (psx_gpu->current_texture_mask & psx_gpu->dirty_textures_8bpp_mask))
    pipeline_update_cache(psx_gpu, 8);

  head = pl.head;
  PL_STAT(PLS_SUBMIT);
  if (head - pl_load(pl.tail) >= PIPELINE_SLOTS) {
    PL_STAT(PLS_FULL);
    pipeline_wait_done(head - PIPELINE_SLOTS + 1);
  }
#ifdef PSX_GPU_PIPELINE_STATS
  pl_stats_blocks += psx_gpu->num_blocks;
#endif
  slot = pl.slots + (head % PIPELINE_SLOTS) * PIPELINE_SLOT_BYTES;
  /* The state up to the padding (the handlers' scratch space follows it),
   * then the blocks: about 2 KB a buffer, 1100 buffers a frame. */
  memcpy(slot, psx_gpu, offsetof(psx_gpu_struct, reserved_a));
  memcpy(slot + offsetof(psx_gpu_struct, blocks), psx_gpu->blocks,
         psx_gpu->num_blocks * sizeof(block_struct));
  if (!pl.pend_any) {
    pl.pend_any = 1;
    pl.pend_x0 = psx_gpu->viewport_start_x;
    pl.pend_y0 = psx_gpu->viewport_start_y;
    pl.pend_x1 = psx_gpu->viewport_end_x;
    pl.pend_y1 = psx_gpu->viewport_end_y;
  }
  else {
    pl.pend_x0 = min(pl.pend_x0, psx_gpu->viewport_start_x);
    pl.pend_y0 = min(pl.pend_y0, psx_gpu->viewport_start_y);
    pl.pend_x1 = max(pl.pend_x1, psx_gpu->viewport_end_x);
    pl.pend_y1 = max(pl.pend_y1, psx_gpu->viewport_end_y);
  }
  pl_store(pl.head, head + 1);
  if (pl_load(pl.waiting_consumer)) {
    slock_lock(pl.lock);
    scond_signal(pl.work);
    slock_unlock(pl.lock);
  }
  return 1;
}

/* Queued buffers are tracked by the viewport current when they are queued:
 * blocks set up under another viewport go before it changes. */
static void pipeline_viewport_change(psx_gpu_struct *psx_gpu)
{
  if (pl.active && psx_gpu->num_blocks)
    flush_render_block_buffer(psx_gpu);
}

/* A textured sprite's setup reads its texels (cache or VRAM) and CLUT here. */
static void pipeline_before_texture_read(psx_gpu_struct *psx_gpu)
{
  u32 mode = (psx_gpu->texture_settings >> 7) & 3;
  int wait;
  PL_STAT(PLS_SPRITE);
  if (!pl.pend_any)
    return;
  wait = pipeline_texture_pending(psx_gpu, mode);
  if (!wait && mode < TEXTURE_MODE_16BPP) {
    u32 offset = psx_gpu->clut_ptr - psx_gpu->vram_ptr;
    u32 y = offset >> 10;
    wait = pipeline_pending(offset & 1023, mode == TEXTURE_MODE_4BPP ? 16 : 256, y, y);
  }
  if (wait)
    pipeline_wait_idle(psx_gpu);
}

static void pipeline_stop(void)
{
  if (!pl.thread)
    return;
  pipeline_wait_idle(&egpu);
  pl.active = 0;
  slock_lock(pl.lock);
  pl.quit = 1;
  scond_signal(pl.work);
  slock_unlock(pl.lock);
  sthread_join(pl.thread);
  pl.thread = NULL;
  scond_free(pl.work);
  scond_free(pl.done);
  slock_free(pl.lock);
  free(pl.slots_alloc);
  memset(&pl, 0, sizeof(pl));
}

static void pipeline_start(void)
{
  if (pl.thread)
    return;
  memset(&pl, 0, sizeof(pl));
  pl.slots_alloc = malloc(PIPELINE_SLOTS * PIPELINE_SLOT_BYTES + 255);
  pl.lock = slock_new();
  pl.work = scond_new();
  pl.done = scond_new();
  if (!pl.slots_alloc || !pl.lock || !pl.work || !pl.done)
    goto fail;
  pl.slots = (u8 *)(((uintptr_t)pl.slots_alloc + 255) & ~(uintptr_t)255);
  pl.thread = pcsxr_sthread_create(pipeline_thread, PCSXRT_GPU2);
  if (!pl.thread)
    goto fail;
  pl.active = 1;
  SysPrintf("gpu_neon: two-stage renderer on\n");
  return;
fail:
  SysPrintf("gpu_neon: two-stage renderer unavailable\n");
  if (pl.work) scond_free(pl.work);
  if (pl.done) scond_free(pl.done);
  if (pl.lock) slock_free(pl.lock);
  free(pl.slots_alloc);
  memset(&pl, 0, sizeof(pl));
}
#endif

int renderer_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
 int *cycles_sum, int *cycles_last, int *last_cmd)
{
  int ret;
  uint64_t started = pcsx_profile_begin(PCSX_PROF_GPU_LIST, 31);

  if (gpu.state.enhancement_active)
    ret = gpu_parse_enhanced(&egpu, list, count * 4, ex_regs,
            cycles_sum, cycles_last, (u32 *)last_cmd);
  else
    ret = gpu_parse(&egpu, list, count * 4, ex_regs,
            cycles_sum, cycles_last, (u32 *)last_cmd);

#ifdef PCSX_SYSTEM12
  ex_regs[1] &= gpu_get_model() == 1 ? ~0x3fff : ~0x1fff;
  ex_regs[1] |= gpu_tpage_from_renderer(egpu.texture_settings);
#else
  ex_regs[1] &= ~0x1ff;
  ex_regs[1] |= egpu.texture_settings & 0x1ff;
#endif
  pcsx_profile_end(PCSX_PROF_GPU_LIST, started);
  return ret;
}

#define ENHANCEMENT_BUF_SIZE (1024 * 1024 * 2 * 4 + 4096 * 2)

static void *get_enhancement_bufer(int *x, int *y, int *w, int *h,
 int *vram_h)
{
  uint16_t *ret = select_enhancement_buf_ptr(&egpu, *x, *y);
  if (ret == NULL)
    return NULL;

  *x *= 2;
  *y *= 2;
  *w = *w * 2;
  *h = *h * 2;
  *vram_h = 1024;
  return ret;
}

static void map_enhancement_buffer(void)
{
  // currently we use 4x 1024*1024 buffers instead of single 2048*1024
  // to be able to reuse 1024-width code better (triangle setup,
  // dithering phase, lines).
  egpu.enhancement_buf_ptr = gpu.mmap(ENHANCEMENT_BUF_SIZE);
  if (egpu.enhancement_buf_ptr == NULL || egpu.enhancement_buf_ptr == (void *)(intptr_t)-1) {
    SysPrintf("failed to map enhancement buffer\n");
    egpu.enhancement_buf_ptr = NULL;
    gpu.get_enhancement_bufer = NULL;
  }
  else {
    egpu.enhancement_buf_ptr += 4096 / 2;
    gpu.get_enhancement_bufer = get_enhancement_bufer;
  }
}

int renderer_init(void)
{
  if (gpu.vram != NULL) {
    initialize_psx_gpu(&egpu, gpu.vram);
    initialized = 1;
  }

  if (gpu.mmap != NULL && egpu.enhancement_buf_ptr == NULL)
    map_enhancement_buffer();

  return 0;
}

void renderer_finish(void)
{
#ifdef PSX_GPU_PIPELINE
  pipeline_stop();
#endif
  if (egpu.enhancement_buf_ptr != NULL) {
    egpu.enhancement_buf_ptr -= 4096 / 2;
    gpu.munmap(egpu.enhancement_buf_ptr, ENHANCEMENT_BUF_SIZE);
  }
  egpu.enhancement_buf_ptr = NULL;
  egpu.enhancement_current_buf_ptr = NULL;
  initialized = 0;
}

static noinline void sync_enhancement_buffers(int x, int y, int w, int h)
{
  int i, right = x + w, bottom = y + h;
  const u16 *src = gpu.vram;
  // use these because the scanout struct may hold reduced w, h
  // due to intersection stuff, see the update_enhancement_buf_scanouts() mess
  int s_w = max(gpu.screen.hres, gpu.screen.w);
  int s_h = gpu.screen.vres;
  if (gpu.screen.y < 0)
    s_h -= gpu.screen.y;
  s_w = min(s_w, 512);
  for (i = 0; i < ARRAY_SIZE(egpu.enhancement_scanouts); i++) {
    const struct psx_gpu_scanout *s = &egpu.enhancement_scanouts[i];
    u16 *dst = select_enhancement_buf_by_index(&egpu, i);
    int x1, x2, y1, y2;
    if (s->w == 0) continue;
    if (s->x >= right) continue;
    if (s->x + s_w <= x) continue;
    if (s->y >= bottom) continue;
    if (s->y + s_h <= y) continue;
    x1 = max(x, s->x);
    x2 = min(right, s->x + s_w);
    y1 = max(y, s->y);
    y2 = min(bottom, s->y + s_h);
    // 16-byte align for the asm version
    x2 += x1 & 7;
    x1 &= ~7;
    scale2x_tiles8(dst + y1 * 1024*2 + x1 * 2,
        src + y1 * 1024 + x1, (x2 - x1 + 7) / 8u, y2 - y1);
  }
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
  s32 dummy0 = 0;
  u32 dummy1 = 0;
  gpu_parse(&egpu, ecmds + 1, 6 * 4, ecmds, &dummy0, &dummy0, &dummy1);
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
  update_texture_cache_region(&egpu, x, y, x + w - 1, y + h - 1);

  if (gpu.state.enhancement_active) {
    if (state_changed) {
      int vres = gpu.screen.vres;
      if (gpu.screen.y < 0)
        vres -= gpu.screen.y;
      memset(egpu.enhancement_scanouts, 0, sizeof(egpu.enhancement_scanouts));
      egpu.enhancement_scanout_eselect = 0;
      update_enhancement_buf_scanouts(&egpu,
        gpu.screen.src_x, gpu.screen.src_y, gpu.screen.hres, vres);
      return;
    }
    sync_enhancement_buffers(x, y, w, h);
  }
}

void renderer_flush_queues(void)
{
  uint64_t started = pcsx_profile_begin(PCSX_PROF_GPU_FLUSH, 1);
  flush_render_block_buffer(&egpu);
#ifdef PSX_GPU_PIPELINE
  PL_STAT(PLS_FLUSH);
  pipeline_wait_idle(&egpu);
#endif
  pcsx_profile_end(PCSX_PROF_GPU_FLUSH, started);
}

void renderer_set_interlace(int enable, int is_odd)
{
  egpu.render_mode &= ~(RENDER_INTERLACE_ENABLED|RENDER_INTERLACE_ODD);
  if (enable)
    egpu.render_mode |= RENDER_INTERLACE_ENABLED;
  if (is_odd)
    egpu.render_mode |= RENDER_INTERLACE_ODD;
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
  int x = screen->src_x, y = screen->src_y;
  int vres = screen->vres;
  if (!gpu.state.enhancement_active || !egpu.enhancement_buf_ptr)
    return;

  if (screen->y < 0)
    vres -= screen->y;
  update_enhancement_buf_scanouts(&egpu, x, y, screen->hres, vres);
}

#include "../../frontend/plugin_lib.h"

void renderer_set_config(const struct rearmed_cbs *cbs)
{
  if (!initialized) {
    initialize_psx_gpu(&egpu, gpu.vram);
    initialized = 1;
  }
  if (cbs->pl_set_gpu_caps)
    cbs->pl_set_gpu_caps(GPU_CAP_SUPPORTS_2X);
  
  egpu.allow_dithering = cbs->dithering;
  egpu.force_dithering = cbs->dithering >> 1;
  /*
  if (!egpu.allow_dithering) {
    egpu.dither_table[0] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[1] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[2] = dither_table_row(0, 0, 0, 0);
    egpu.dither_table[3] = dither_table_row(0, 0, 0, 0);
  } else {
    egpu.dither_table[0] = dither_table_row(-4, 0, -3, 1);
    egpu.dither_table[1] = dither_table_row(2, -2, 3, -1);
    egpu.dither_table[2] = dither_table_row(-3, 1, -4, 0);
    egpu.dither_table[3] = dither_table_row(3, -1, 2, -2); 
  }
  */

#ifdef PSX_GPU_PIPELINE
  if (psx_gpu_pipeline_requested)
    pipeline_start();
  else
    pipeline_stop();
#endif
  egpu.hack_disable_main = cbs->gpu_neon.enhancement_no_main;
  egpu.hack_texture_adj = cbs->gpu_neon.enhancement_tex_adj;
  if (gpu.state.enhancement_enable) {
    if (gpu.mmap != NULL && egpu.enhancement_buf_ptr == NULL)
      map_enhancement_buffer();
  }
}

// vim:ts=2:sw=2:expandtab
