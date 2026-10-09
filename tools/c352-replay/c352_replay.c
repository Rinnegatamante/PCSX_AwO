/*
 * Replays a C352 call trace recorded by c352.c built with -DC352_TRACE
 * (C352_TRACE_FILE=path:frames): register writes and reads, resets and
 * generate calls with their sizes, against the same wave ROM. Checks the
 * output against the recorded FNV-1a and times the generation, so the chip
 * can be optimised and profiled (callgrind in the ARMv7 container) alone.
 *
 *   c352-replay TRACE WAVE_ROM [--loops N] [--max-frames N]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../libpcsxcore/arcade/c352.h"

enum { C352T_WRITE = 1, C352T_READ, C352T_GENERATE, C352T_RESET, C352T_END };

static void *load(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  void *buf;
  long n;
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc(n ? n : 1);
  if (!buf || fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "read %s failed\n", path); exit(1); }
  fclose(f);
  *size = n;
  return buf;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
  const uint8_t *b = p;
  while (n--)
    h = (h ^ *b++) * 0x100000001b3ull;
  return h;
}

static double now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
  size_t tsize, rsize, n, pos;
  uint32_t *t;
  uint8_t *rom;
  uint64_t rom_hash, want = 0, got = 0;
  int loops = 1, loop, have_end = 0, i;
  unsigned long long max_frames = ~0ull, produced;
  unsigned long long frames_total = 0, calls = 0, hist[10] = { 0 };
  static int16_t out[65536 * 4];
  double t0, ms;

  if (argc < 3) {
    fprintf(stderr, "usage: %s TRACE WAVE_ROM [--loops N]\n", argv[0]);
    return 1;
  }
  for (i = 3; i < argc; i++)
    if (!strcmp(argv[i], "--loops") && i + 1 < argc)
      loops = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc)   /* stop early: no check */
      max_frames = strtoull(argv[++i], NULL, 0);
  t = load(argv[1], &tsize);
  rom = load(argv[2], &rsize);
  n = tsize / 4;
  if (n < 8 || t[0] != 0x54323533 || t[3] != rsize) {
    fprintf(stderr, "not a C352 trace for this ROM size\n");
    return 1;
  }
  rom_hash = t[4] | ((uint64_t)t[5] << 32);
  if (fnv(0xcbf29ce484222325ull, rom, rsize) != rom_hash) {
    fprintf(stderr, "wave ROM differs from the recorded one\n");
    return 1;
  }
  t0 = now();
  for (loop = 0; loop < loops; loop++) {
    C352 *c = c352_create(rom, rsize, t[1], t[2]);
    got = 0xcbf29ce484222325ull;
    produced = 0;
    for (pos = 8; pos + 4 <= n && produced < max_frames; pos += 4) {
      uint32_t tag = t[pos], a = t[pos + 1], b = t[pos + 2], d = t[pos + 3];
      if (tag == C352T_WRITE)
        c352_write(c, a, (uint16_t)b, (uint16_t)d);
      else if (tag == C352T_READ)
        c352_read(c, a);
      else if (tag == C352T_RESET)
        c352_reset(c);
      else if (tag == C352T_GENERATE) {
        if (a > 65536) { fprintf(stderr, "generate of %u frames\n", a); return 1; }
        c352_generate(c, out, a);
        produced += a;
        if (max_frames != ~0ull)
          continue;
        got = fnv(got, out, (size_t)a * 8);
        if (loop == 0) {
          unsigned k = 0;
          while (k < 9 && (1u << (k + 1)) <= a) k++;
          hist[k]++;
          frames_total += a;
          calls++;
        }
      }
      else if (tag == C352T_END) {
        want = a | ((uint64_t)b << 32);
        have_end = 1;
        break;
      }
      else { fprintf(stderr, "bad record %u\n", tag); return 1; }
    }
    c352_destroy(c);
  }
  ms = (now() - t0) * 1000.0 / loops;
  printf("c352-replay: %llu calls, %llu frames, sizes", calls, frames_total);
  for (i = 0; i < 10; i++)
    if (hist[i]) printf(" %s%u:%llu", i == 9 ? ">=" : "", 1u << i, hist[i]);
  printf("\nc352-replay: output=%016llx %s %.3f ms/loop\n", (unsigned long long)got,
         !have_end ? "(no recorded hash)" : got == want ? "MATCH" : "MISMATCH", ms);
  return have_end && got != want;
}
