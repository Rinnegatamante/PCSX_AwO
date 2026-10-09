/*
 * Replays a GPU command trace recorded by gpulib built with -DGPULIB_TRACE
 * (GPULIB_TRACE_FILE=path:from:to): loads the VRAM/register snapshot, then
 * makes the same GPU entry calls, so only gpulib and the renderer run. Used to
 * profile the renderer (callgrind in the ARMv7 container runs the NEON
 * assembly) and to check that renderer changes keep every displayed frame
 * and the final VRAM byte-identical.
 *
 *   gpu-replay TRACE [--loops N] [--hash FILE] [--no-frame-hash] [--dump-vram FILE]
 *
 * Trace: 32-word header (magic "SGTR", version 1, VRAM rows, GPU model, from,
 * to, cbs fields, start frame, PSX RAM size), status word, 256 control words,
 * VRAM; then records of three words {tag, a, b} with payloads (see gpu.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../plugins/gpulib/gpu.h"
#include "../../libpcsxcore/gpu.h"
#include "../../frontend/plugin_lib.h"

enum {
  GT_STATUS = 1, GT_DATA_MEM, GT_DATA, GT_CHAIN, GT_CHAIN_PKT, GT_CHAIN_END,
  GT_READ_MEM, GT_READ, GT_READ_STATUS, GT_VBLANK, GT_END
};

int gpu_set_model(int model);
int gpu_set_vram_height(unsigned rows);
unsigned gpu_get_vram_height(void);
#ifdef PCSX_SYSTEM12
uint32_t psxRamSize = 0x200000, psxRomSize = 0x80000;
#endif

#include <stdarg.h>
void SysPrintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static unsigned frame_count, hcnt;
static struct rearmed_cbs cbs;
static uint64_t frame_hash = 0xcbf29ce484222325ull;
static unsigned flips;
static FILE *hash_out;
static int no_frame_hash;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
  const uint8_t *b = p;
  while (n--)
    h = (h ^ *b++) * 0x100000001b3ull;
  return h;
}

static int vout_open(void) { return 0; }
static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
  (void)w; (void)h; (void)raw_w; (void)raw_h; (void)bpp;
}
static void vout_close(void) {}

/* The displayed rectangle of every flip, as the frontend would read it. */
static void vout_flip(const void *vram, int vram_ofs, int bgr24, int x, int y,
                      int w, int h, int dims_changed)
{
  const uint16_t *v = vram;
  uint64_t f = 0xcbf29ce484222325ull;
  int stride = 1024, row;
  (void)x; (void)y; (void)dims_changed;
  if (vram && !no_frame_hash) {
    for (row = 0; row < h; row++) {
      int ofs = (vram_ofs + row * stride) & (1024 * gpu_get_vram_height() - 1);
      f = fnv(f, v + ofs, (size_t)w * (bgr24 ? 3 : 2));
    }
  }
  if (hash_out)
    fprintf(hash_out, "%u %dx%d %016llx\n", flips, w, h, (unsigned long long)f);
  frame_hash = fnv(frame_hash, &f, sizeof(f));
  flips++;
}

static void *vram_mmap(unsigned int size) { return calloc(1, size); }
static void vram_munmap(void *p, unsigned int size) { (void)size; free(p); }
static void state_change(int what, int cycles) { (void)what; (void)cycles; }

static uint32_t *load(const char *path, size_t *words)
{
  FILE *f = fopen(path, "rb");
  long n;
  uint32_t *buf;
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc(n);
  if (!buf || fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "read %s failed\n", path); exit(1); }
  fclose(f);
  *words = n / 4;
  return buf;
}

static double now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
  size_t words, pos, vram_words;
  uint32_t *t, *hdr, *ram;
  int loops = 1, loop, i;
  uint16_t *vram = NULL;
  GPUFreeze_t *fz;
  double t0;
  uint64_t end_hash = 0, vram_hash;
  const char *dump_path = NULL;
  int have_end = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s TRACE [--loops N] [--hash FILE]\n", argv[0]);
    return 1;
  }
  for (i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--loops") && i + 1 < argc)
      loops = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--hash") && i + 1 < argc)
      hash_out = fopen(argv[++i], "w");
    else if (!strcmp(argv[i], "--no-frame-hash"))   /* profiling: skip the flip hashes */
      no_frame_hash = 1;
    else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc)
      dump_path = argv[++i];
  }
  t = load(argv[1], &words);
  hdr = t;
  if (words < 32 + 257 || hdr[0] != 0x52544753 || hdr[1] != 1) {
    fprintf(stderr, "not a version 1 GPU trace\n");
    return 1;
  }
#ifdef PCSX_SYSTEM12
  psxRamSize = hdr[24];
#endif
  if ((hdr[3] != 2 && gpu_set_model(hdr[3]) != 0) ||
      (hdr[2] != gpu_get_vram_height() && gpu_set_vram_height(hdr[2]) != 0)) {
    fprintf(stderr, "GPU model %u / %u VRAM rows unsupported by this build\n", hdr[3], hdr[2]);
    return 1;
  }
  vram_words = 1024 * hdr[2] / 2;
  ram = calloc(1, hdr[24]);
  fz = calloc(1, sizeof(*fz));

  cbs.pl_vout_open = vout_open;
  cbs.pl_vout_set_mode = vout_set_mode;
  cbs.pl_vout_flip = vout_flip;
  cbs.pl_vout_close = vout_close;
  cbs.mmap = vram_mmap;
  cbs.munmap = vram_munmap;
  cbs.gpu_state_change = state_change;
  cbs.gpu_frame_count = &frame_count;
  cbs.gpu_hcnt = &hcnt;
  cbs.frameskip = hdr[6]; cbs.dithering = hdr[7]; cbs.scale_hires = hdr[8];
  cbs.alt_flip = hdr[9]; cbs.gpu_hacks = hdr[10];
  cbs.gpu_neon.allow_interlace = hdr[11]; cbs.gpu_neon.enhancement_enable = hdr[12];
  cbs.gpu_neon.enhancement_no_main = hdr[13]; cbs.gpu_neon.enhancement_tex_adj = hdr[14];
  cbs.screen_centering_type = hdr[15]; cbs.screen_centering_type_default = hdr[16];
  cbs.screen_centering_x = hdr[17]; cbs.screen_centering_y = hdr[18];
  cbs.screen_centering_h_adj = hdr[19]; cbs.show_overscan = hdr[20];
  cbs.only_16bpp = hdr[21]; cbs.gpu_caps = hdr[22];
  cbs.thread_rendering = 0;
  frame_count = hdr[23];

  GPUinit();
  GPUrearmedCallbacks(&cbs);

  t0 = now();
  for (loop = 0; loop < loops; loop++) {
    /* Snapshot: VRAM first, then the registers through GPUfreeze's load
     * path, which also rebuilds the renderer's caches from that VRAM. */
    GPUfreeze(1, fz, &vram);
    memcpy(vram, t + 32 + 257, vram_words * 4);
    fz->ulStatus = t[32];
    memcpy(fz->ulControl, t + 33, 256 * 4);
    GPUfreeze(0, fz, &vram);
    frame_count = hdr[23];
    frame_hash = 0xcbf29ce484222325ull;
    flips = 0;

    for (pos = 32 + 257 + vram_words; pos + 3 <= words; ) {
      uint32_t tag = t[pos], a = t[pos + 1], b = t[pos + 2];
      pos += 3;
      switch (tag) {
      case GT_STATUS: GPUwriteStatus(a); break;
      case GT_DATA_MEM: GPUwriteDataMem(t + pos, a); pos += a; break;
      case GT_DATA: GPUwriteData(a); break;
      case GT_CHAIN: {
        uint32_t progress = 0;
        int32_t cycles_last = 0;
        /* Lay the walked packets back where they were, then walk them. */
        while (pos + 3 <= words && t[pos] == GT_CHAIN_PKT) {
          uint32_t ofs = t[pos + 1], len = t[pos + 2];
          memcpy((uint8_t *)ram + (ofs & (hdr[24] - 1)), t + pos + 3, (len + 1) * 4);
          pos += 3 + len + 1;
        }
        if (pos + 3 > words || t[pos] != GT_CHAIN_END) {
          fprintf(stderr, "truncated DMA chain at word %zu\n", pos);
          return 1;
        }
        pos += 3;
        GPUdmaChain(ram, a, b ? &progress : NULL, &cycles_last);
        break;
      }
      case GT_READ_MEM: {
        static uint32_t sink[1 << 20];
        GPUreadDataMem(sink, a > (1u << 20) ? (1u << 20) : a);
        break;
      }
      case GT_READ: GPUreadData(); break;
      case GT_READ_STATUS: GPUreadStatus(); break;
      case GT_VBLANK:
        frame_count = b;
        hcnt = t[pos++];
        GPUvBlank(a & 1, a >> 1);
        break;
      case GT_END:
        end_hash = a | ((uint64_t)b << 32);
        have_end = 1;
        pos = words;
        break;
      default:
        fprintf(stderr, "bad record %u at word %zu\n", tag, pos - 3);
        return 1;
      }
    }
    if (hash_out) {
      fclose(hash_out);
      hash_out = NULL;
    }
  }
  double ms = (now() - t0) * 1000.0 / loops;
  GPUfreeze(1, fz, &vram);
  vram_hash = fnv(0xcbf29ce484222325ull, vram, vram_words * 4);
  if (dump_path) {
    FILE *vf = fopen(dump_path, "wb");
    if (vf) {
      fwrite(vram, 4, vram_words, vf);
      fclose(vf);
    }
  }
  printf("gpu-replay: loops=%d flips=%u frames=%016llx vram=%016llx %s %.3f ms/loop\n",
         loops, flips, (unsigned long long)frame_hash, (unsigned long long)vram_hash,
         !have_end ? "(no recorded VRAM)" : vram_hash == end_hash ? "MATCH" : "MISMATCH", ms);
  return have_end && vram_hash != end_hash;
}
