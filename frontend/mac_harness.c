/* Headless native macOS harness, GPL-2.0-or-later. */
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include "../libpcsxcore/r3000a.h"
#include "../libpcsxcore/psxinterpreter.h"
#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/psxcounters.h"
#include "../libpcsxcore/spu.h"
#include "../libpcsxcore/gpu.h"
#include "../plugins/dfsound/spu_config.h"
#include "plugin.h"
#include "plugin_lib.h"
#include "gpu_vram.h"
#include "../libpcsxcore/psxmem.h"
#ifdef PCSX_SYSTEM12
#include "../libpcsxcore/system12.h"
#include "../libpcsxcore/system11.h"
#include "../libpcsxcore/system10.h"
#include "../libpcsxcore/systemzn.h"
#include "../libpcsxcore/systemtaito.h"

enum { BOARD_BUS_TRACE_BIN_CYCLES = 8192, BOARD_BUS_TRACE_MAX_BINS = 100000 };
typedef struct harness_bus_trace_bin {
  uint64_t hash;
  uint32_t count, first_cycle, first_address, first_value, first_pc;
  uint8_t first_width, first_write;
} harness_bus_trace_bin;
static void harness_system12_bus_event(void *opaque, u32 address, unsigned width,
    int is_write, u32 value, u32 pc, u32 cycle)
{
  harness_bus_trace_bin *bins = opaque;
  uint32_t index = cycle / BOARD_BUS_TRACE_BIN_CYCLES;
  harness_bus_trace_bin *bin;
  uint64_t fields[2];
  unsigned i, j;
  if (!bins || index >= BOARD_BUS_TRACE_MAX_BINS) return;
  /* Ignore writes to MAME's declared no-op windows. They dominate the
   * early BIOS wait loop but cannot distinguish the board's real buses. */
  if (is_write && ((address >= 0x1f008000 && address < 0x1f008004) ||
      (address >= 0x1f010000 && address < 0x1f010004) ||
      (address >= 0x1f018000 && address < 0x1f018004) ||
      (address >= 0x1f1bff08 && address < 0x1f1bff10) ||
      (address >= 0x1f000002 && address < 0x1f000004))) return;
  bin = &bins[index];
  if (!bin->count) {
    bin->hash = UINT64_C(14695981039346656037);
    bin->first_cycle = cycle;
    bin->first_address = address;
    bin->first_value = value;
    bin->first_pc = pc;
    bin->first_width = width > 255 ? 255 : width;
    bin->first_write = is_write != 0;
  }
  if (bin->count != UINT32_MAX) bin->count++;
  fields[0] = address | ((uint64_t)(width & 0xff) << 32) |
    ((uint64_t)(is_write != 0) << 40);
  fields[1] = value;
  for (i = 0; i < 2; i++)
    for (j = 0; j < sizeof(fields[i]); j++) {
      bin->hash ^= (fields[i] >> (j * 8)) & 0xff;
      bin->hash *= UINT64_C(1099511628211);
    }
}

static int write_system12_bus_trace(FILE *file,
    const harness_bus_trace_bin *bins)
{
  unsigned i;
  if (!file || !bins) return 0;
  fputs("bin,cycle,events,fnv64,address,width,write,value,reported_pc\n", file);
  for (i = 0; i < BOARD_BUS_TRACE_MAX_BINS; i++) {
    const harness_bus_trace_bin *b = &bins[i];
    if (b->count)
      fprintf(file, "%u,%u,%u,%016" PRIx64 ",%08x,%u,%u,%08x,%08x\n",
        i, b->first_cycle, b->count, b->hash, b->first_address,
        b->first_width, b->first_write, b->first_value, b->first_pc);
  }
  return ferror(file) ? -1 : 0;
}

typedef struct HarnessBoard {
  int family;
  const char *name;
  int (*load)(const char *);
  void (*unload)(void);
  int (*sync)(u32);
  int (*set_input)(const char *, int);
  int (*selftest)(void);
  void (*report)(FILE *);
  int (*open_audio)(const char *);
  int (*close_audio)(void);
  int (*import_nvram)(const unsigned char *, size_t);
  int (*export_nvram)(unsigned char *, size_t);
} HarnessBoard;
static int sync_system12(u32 cycle) { system12_sync(cycle); return 0; }
static const HarnessBoard boards[] = {
  {101, "TaitoFX1S", systemtaito_load, systemtaito_unload, systemtaito_sync,
    systemtaito_set_input, systemtaito_selftest, systemtaito_report,
    systemtaito_open_audio, systemtaito_close_audio, systemtaito_eeprom_import, systemtaito_eeprom_export},
  {100, "CapcomZN", systemzn_load, systemzn_unload, systemzn_sync,
    systemzn_set_input, systemzn_selftest, systemzn_report,
    systemzn_open_audio, systemzn_close_audio, systemzn_eeprom_import, systemzn_eeprom_export},
  {10, "System10", system10_load, system10_unload, system10_sync,
    system10_set_input, system10_selftest, system10_report, NULL, NULL, NULL, NULL},
  {11, "System11", system11_load, system11_unload, system11_sync,
    system11_set_input, system11_selftest, system11_report,
    system11_open_audio, system11_close_audio, system11_eeprom_import, system11_eeprom_export},
  {12, "System12", system12_load, system12_unload, sync_system12,
    system12_set_input, system12_selftest, system12_report,
    system12_open_audio, system12_close_audio, system12_eeprom_import, system12_eeprom_export},
};
static const HarnessBoard *selected_board;
#endif

#if !defined(__APPLE__) || !defined(__aarch64__)
#error This harness targets native macOS ARM64.
#endif
#if !defined(__ARM_NEON) && !defined(__ARM_NEON__)
#error GPU_NEON needs ARM Advanced SIMD.
#endif

int in_type[8] = { PSE_PAD_TYPE_STANDARD, PSE_PAD_TYPE_STANDARD };
int multitap1, multitap2, in_analog_left[8][2], in_analog_right[8][2];
unsigned short in_keystate[8];
int in_mouse[8][2];
struct rearmed_cbs pl_rearmed_cbs;
void GPUrearmedCallbacks(const struct rearmed_cbs *cbs);
int harness_write_ppm(const char *path, int w, int h);
unsigned short harness_vram_pixel(int x, int y);
void harness_capture_mode(int w, int h);
void harness_capture_flip(const void *vram, int offset, int rgb24,
                          int x, int y, int w, int h);
static volatile sig_atomic_t interrupted;
static unsigned int flips;
static void on_signal(int sig) { interrupted = sig; }
static int video_open(void) { return 0; }
static void video_close(void) {}
static void video_mode(int w, int h, int rw, int rh, int bpp)
{
  harness_capture_mode(w, h);
}
static void video_flip(const void *vram, int offset, int bgr24,
                       int x, int y, int w, int h, int changed)
{
  harness_capture_flip(vram, offset, bgr24, x, y, w, h);
  flips++;
}
static void *gpu_alloc(unsigned int size)
{
  void *ptr = NULL;
  if (posix_memalign(&ptr, 4096, size)) return NULL;
  memset(ptr, 0, size);
  return ptr;
}
static void gpu_free(void *ptr, unsigned int size) { free(ptr); }
void pl_frame_limit(void) {}
void pl_timing_prepare(int is_pal) {}
void plat_trigger_vibrate(int pad, int low, int high) {}
void pl_gun_byte2(int gun, unsigned char byte) {}
void SysPrintf(const char *fmt, ...)
{
  va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
}
void SysMessage(const char *fmt, ...)
{
  va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
  fputc('\n', stderr);
}
void *SysLoadLibrary(const char *path)
{
  return (void *)(uintptr_t)(PLUGIN_DL_BASE + (strstr(path, "spu") ? PLUGIN_SPU : PLUGIN_GPU));
}
void *SysLoadSym(void *lib, const char *sym)
{
  return plugin_link((uintptr_t)lib - PLUGIN_DL_BASE, sym);
}
const char *SysLibError(void) { return NULL; }
void SysCloseLibrary(void *lib) {}
void SysRunGui(void) { interrupted = 1; }

static void usage(const char *name)
{
  fprintf(stderr, "usage: %s (--exe PS-X-EXE | --system12 REGION-DIR | --system11 REGION-DIR | --system10 REGION-DIR | --capcom-zn REGION-DIR | --taito-fx1 REGION-DIR) "
    "[--frames N] [--output FRAME.ppm] [--board-test] [--verify-smoke] "
    "[--inputs FILE.csv] [--audio FILE.wav] [--nvram FILE] "
    "[--audio-test-fixture] "
    "[--trace FILE --trace-blocks N] [--cycle-snapshots FILE "
    "--snapshot-start CYCLES --snapshot-interval N] "
    "[--board-bus-trace FILE] "
    "[--max-blocks N] [--icache] [--drc]\n"
    "       %s (--gpu-type1-test | --gpu-type1-test-1m)\n", name, name);
}

#ifdef PCSX_SYSTEM12
static int load_nvram(const char *path)
{
  unsigned char image[2080];
  FILE *file = fopen(path, "rb");
  int invalid;
  if (!file) {
    if (errno == ENOENT) return 0;
    perror(path); return -1;
  }
  invalid = fread(image, 1, sizeof(image), file) != sizeof(image);
  if (!invalid) invalid = fgetc(file) != EOF || ferror(file);
  if (fclose(file)) invalid = 1;
  if (invalid) {
    fprintf(stderr, "%s: expected exact 2080-byte AT28C16 image\n", path);
    return -1;
  }
  return selected_board->import_nvram(image, sizeof(image));
}

static int save_nvram(const char *path)
{
  unsigned char image[2080];
  char *temporary;
  FILE *file;
  int fd, result = -1;
  if (selected_board->export_nvram(image, sizeof(image))) return -1;
  temporary = malloc(strlen(path) + sizeof(".tmp.XXXXXX"));
  if (!temporary) return -1;
  sprintf(temporary, "%s.tmp.XXXXXX", path);
  fd = mkstemp(temporary);
  if (fd < 0) { perror(path); free(temporary); return -1; }
  file = fdopen(fd, "wb");
  if (file) {
    int invalid = fwrite(image, 1, sizeof(image), file) != sizeof(image);
    if (fclose(file)) invalid = 1;
    if (!invalid && !rename(temporary, path)) result = 0;
  }
  else close(fd);
  if (result) {
    fprintf(stderr, "%s: failed to save AT28C16 image\n", path);
    unlink(temporary);
  }
  free(temporary);
  return result;
}

typedef struct harness_input_event {
  uint64_t frame;
  char control[32];
  int pressed;
} harness_input_event;

static int input_control_known(const char *name)
{
  /* Validate against the selected source input profile while every control
   * is still released, before the CPU begins executing CSV events. */
  return strlen(name) < sizeof(((harness_input_event *)0)->control) &&
    selected_board && selected_board->set_input(name, 0) == 0;
}

static char *trim_field(char *s)
{
  char *end;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
         end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
  return s;
}

static int load_input_events(const char *path, harness_input_event **out,
                             size_t *count)
{
  FILE *f = fopen(path, "r");
  char *line = NULL;
  size_t linecap = 0, used = 0, cap = 0;
  harness_input_event *events = NULL;
  unsigned long long last_frame = 0;
  int have_frame = 0, failed = 0;
  ssize_t n;
  if (!f) { perror(path); return -1; }
  while ((n = getline(&line, &linecap, f)) >= 0) {
    char *frame_s, *control, *pressed_s, *end;
    unsigned long long frame;
    unsigned long pressed;
    (void)n;
    frame_s = trim_field(line);
    if (!*frame_s || *frame_s == '#') continue;
    control = strchr(frame_s, ',');
    if (!control) { failed = 1; break; }
    *control++ = 0;
    pressed_s = strchr(control, ',');
    if (!pressed_s) { failed = 1; break; }
    *pressed_s++ = 0;
    if (strchr(pressed_s, ',')) { failed = 1; break; }
    frame_s = trim_field(frame_s); control = trim_field(control);
    pressed_s = trim_field(pressed_s);
    if (!strcmp(frame_s, "frame") && !strcmp(control, "control") &&
        !strcmp(pressed_s, "pressed")) continue;
    errno = 0; frame = strtoull(frame_s, &end, 10);
    if (errno || end == frame_s || *end || frame > 216000 ||
        (have_frame && frame < last_frame) || !input_control_known(control)) {
      failed = 1; break;
    }
    errno = 0; pressed = strtoul(pressed_s, &end, 10);
    if (errno || end == pressed_s || *end || pressed > 1) { failed = 1; break; }
    if (used == cap) {
      size_t newcap = cap ? cap * 2 : 32;
      harness_input_event *next = realloc(events, newcap * sizeof(*events));
      if (!next) { failed = 1; break; }
      events = next; cap = newcap;
    }
    events[used].frame = frame;
    snprintf(events[used].control, sizeof(events[used].control), "%s", control);
    events[used].pressed = (int)pressed;
    used++; last_frame = frame; have_frame = 1;
  }
  if (ferror(f)) failed = 1;
  free(line);
  if (fclose(f)) failed = 1;
  if (failed) {
    fprintf(stderr, "%s: invalid inputs CSV (expected ordered frame,control,pressed rows)\n", path);
    free(events); return -1;
  }
  *out = events; *count = used;
  return 0;
}

static int apply_input_events(const harness_input_event *events, size_t count,
                              size_t *next, uint64_t frame)
{
  while (*next < count && events[*next].frame <= frame) {
    if (selected_board->set_input(events[*next].control, events[*next].pressed)) {
      fprintf(stderr, "invalid arcade control: %s\n", events[*next].control);
      return -1;
    }
    (*next)++;
  }
  return 0;
}
#endif

int main(int argc, char **argv)
{
  const char *exe = NULL, *board = NULL, *output = "frame.ppm";
  const char *inputs_path = NULL, *audio_path = NULL, *nvram_path = NULL;
#ifdef PCSX_SYSTEM12
  harness_input_event *input_events = NULL;
  size_t input_event_count = 0, next_input_event = 0;
#endif
  const char *trace_path = NULL;
  FILE *trace = NULL;
  const char *snapshot_path = NULL;
  FILE *snapshots = NULL;
  const char *bus_trace_path = NULL;
  FILE *bus_trace_file = NULL;
#ifdef PCSX_SYSTEM12
  harness_bus_trace_bin *bus_trace_bins = NULL;
  int bus_trace_finished = 0;
#endif
  uint64_t snapshot_interval = 262144, snapshot_start = 262144, next_snapshot = 262144;
  unsigned int snapshot_count = 0;
  unsigned int frames = 3, start_frame, prev_cycle;
  uint64_t cycles = 0, cycle_limit;
  unsigned long blocks = 0, trace_blocks = 1000, max_blocks = 1000000000;
  int i, board_test = 0, audio_test_fixture = 0, verify = 0, result = 0, gpu_type1_test = 0;
  int board_family = 12;
  int icache = 0;
  int use_drc = 0;
  struct timespec begin, end;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
    else if ((!strcmp(argv[i], "--system12") || !strcmp(argv[i], "--system11") ||
        !strcmp(argv[i], "--system10") || !strcmp(argv[i], "--capcom-zn") ||
        !strcmp(argv[i], "--taito-fx1")) && i + 1 < argc) {
      if (board) { usage(argv[0]); return 2; }
      board_family = !strcmp(argv[i], "--system11") ? 11 :
        !strcmp(argv[i], "--system10") ? 10 : !strcmp(argv[i], "--capcom-zn") ? 100 :
        !strcmp(argv[i], "--taito-fx1") ? 101 : 12;
      board = argv[++i];
    }
    else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs_path = argv[++i];
    else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_path = argv[++i];
    else if (!strcmp(argv[i], "--nvram") && i + 1 < argc) nvram_path = argv[++i];
    else if (!strcmp(argv[i], "--audio-test-fixture")) audio_test_fixture = 1;
    else if (!strcmp(argv[i], "--drc")) use_drc = 1;
    else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
    else if (!strcmp(argv[i], "--cycle-snapshots") && i + 1 < argc) snapshot_path = argv[++i];
    else if (!strcmp(argv[i], "--board-bus-trace") && i + 1 < argc) bus_trace_path = argv[++i];
    else if ((!strcmp(argv[i], "--trace-blocks") || !strcmp(argv[i], "--max-blocks")) && i + 1 < argc) {
      int tracing = !strcmp(argv[i], "--trace-blocks");
      char *tail; unsigned long n;
      errno = 0; n = strtoul(argv[++i], &tail, 10);
      if (errno || *tail || n == 0 || n > (tracing ? 1000000UL : 100000000000UL)) {
        usage(argv[0]); return 2;
      }
      if (tracing) trace_blocks = n; else max_blocks = n;
    }
    else if (!strcmp(argv[i], "--snapshot-interval") && i + 1 < argc) {
      char *tail; unsigned long long n;
      errno = 0; n = strtoull(argv[++i], &tail, 10);
      if (errno || *tail || n == 0 || n > 0x100000000ULL) { usage(argv[0]); return 2; }
      snapshot_interval = n;
    }
    else if (!strcmp(argv[i], "--snapshot-start") && i + 1 < argc) {
      char *tail; unsigned long long n;
      errno = 0; n = strtoull(argv[++i], &tail, 10);
      if (errno || *tail) { usage(argv[0]); return 2; }
      snapshot_start = (uint64_t)n;
    }
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      char *tail; unsigned long n = strtoul(argv[++i], &tail, 10);
      if (*tail || n == 0 || n > 216000) { usage(argv[0]); return 2; }
      frames = n;
    }
    else if (!strcmp(argv[i], "--gpu-type1-test") || !strcmp(argv[i], "--gpu-type1-test-1m")) {
      if (gpu_type1_test) { usage(argv[0]); return 2; }
      gpu_type1_test = !strcmp(argv[i], "--gpu-type1-test-1m") ? 2 : 1;
    }
    else if (!strcmp(argv[i], "--board-test")) board_test = 1;
    else if (!strcmp(argv[i], "--verify-smoke")) verify = 1;
    else if (!strcmp(argv[i], "--icache")) icache = 1;
    else { usage(argv[0]); return 2; }
  }
  next_snapshot = snapshot_start ? snapshot_start : snapshot_interval;
  if ((!exe && !board && !gpu_type1_test) || (exe && board) ||
      (gpu_type1_test && (exe || board || verify || trace_path)) || (board_test && !board) ||
      ((inputs_path || audio_path || audio_test_fixture || nvram_path) && !board) ||
      (snapshot_path && (!board || board_family != 12)) ||
      (bus_trace_path && (!board || board_family != 12)) ||
      (inputs_path && !strcmp(inputs_path, output)) ||
      (trace_path && inputs_path && !strcmp(trace_path, inputs_path)) ||
      (nvram_path && ((!strcmp(nvram_path, output)) ||
        (inputs_path && !strcmp(nvram_path, inputs_path)) ||
        (audio_path && !strcmp(nvram_path, audio_path)) ||
        (trace_path && !strcmp(nvram_path, trace_path)) ||
        (snapshot_path && !strcmp(nvram_path, snapshot_path)) ||
        (bus_trace_path && !strcmp(nvram_path, bus_trace_path)))) ||
      (snapshot_path && ((trace_path && !strcmp(snapshot_path, trace_path)) ||
        (inputs_path && !strcmp(snapshot_path, inputs_path)) ||
        (audio_path && !strcmp(snapshot_path, audio_path)) ||
        !strcmp(snapshot_path, output))) ||
      (bus_trace_path && ((trace_path && !strcmp(bus_trace_path, trace_path)) ||
        (snapshot_path && !strcmp(bus_trace_path, snapshot_path)) ||
        (inputs_path && !strcmp(bus_trace_path, inputs_path)) ||
        (audio_path && !strcmp(bus_trace_path, audio_path)) ||
        !strcmp(bus_trace_path, output))) ||
      (audio_test_fixture && (!board_test || !audio_path || board_family != 12))) {
    usage(argv[0]); return 2;
  }
  if (trace_path) {
    trace = fopen(trace_path, "w");
    if (!trace) { perror(trace_path); return 1; }
    fputs("block,cycle,pc,opcode", trace);
    for (i = 0; i < 32; i++) fprintf(trace, ",r%d", i);
    fputc('\n', trace);
  }
  if (snapshot_path) {
    snapshots = fopen(snapshot_path, "w");
    if (!snapshots) { perror(snapshot_path); if (trace) fclose(trace); return 1; }
    fputs("cycles,frame,pc,opcode,sr,cause,interrupt,i_stat,i_mask,ram_vector80,"
      "ram_d2190,ram_d2194,ram_d2198,ram_d219c,ram_d21a0,ram_d21a4,"
      "ram_d21a8,ram_d21ac,ram_d21b0,ram_d21b4,ram_d21b8,ram_d21bc,"
      "ram_d21c0,ram_d21c4,ram_d21c8,ram_d21cc,ram_d21d0,ram_d21d4,"
      "ram_d21d8,ram_d21dc", snapshots);
    for (i = 0; i < 32; i++) fprintf(snapshots, ",r%d", i);
    fputc('\n', snapshots);
  }
  if (bus_trace_path) {
#ifdef PCSX_SYSTEM12
    bus_trace_bins = calloc(BOARD_BUS_TRACE_MAX_BINS, sizeof(*bus_trace_bins));
    bus_trace_file = fopen(bus_trace_path, "w");
    if (!bus_trace_bins || !bus_trace_file) {
      perror(bus_trace_path);
      free(bus_trace_bins);
      if (trace) fclose(trace);
      if (snapshots) fclose(snapshots);
      if (bus_trace_file) fclose(bus_trace_file);
      return 1;
    }
    fprintf(stderr, "System12 bus trace: %u-cycle bins, %u bins (%" PRIu64
      "-cycle horizon); reported_pc is block state, not instruction-exact under DRC\n",
      BOARD_BUS_TRACE_BIN_CYCLES, BOARD_BUS_TRACE_MAX_BINS,
      (uint64_t)BOARD_BUS_TRACE_BIN_CYCLES * BOARD_BUS_TRACE_MAX_BINS);
#else
    fprintf(stderr, "Rebuild with BOARD=1 for System12 bus tracing.\n");
    return 2;
#endif
  }
  memset(&Config, 0, sizeof(Config));
  Config.HLE = 1;
  Config.Cpu = use_drc ? CPU_DYNAREC : CPU_INTERPRETER;
#ifdef DRC_DISABLE
  if (use_drc) { fprintf(stderr, "Rebuild with DRC=1 for --drc.\n"); return 2; }
#endif
  if (use_drc && board && board_family != 12) {
    fprintf(stderr, "Arcade DRC validation currently covers System 12 only.\n"); return 2;
  }
  Config.icache_emulation = icache;
  Config.cycle_multiplier = CYCLE_MULT_DEFAULT;
  Config.PsxType = PSX_TYPE_NTSC;
  strcpy(Config.Gpu, "builtin_gpu"); strcpy(Config.Spu, "builtin_spu");
  strcpy(Config.Bios[0], "HLE"); strcpy(Config.Bios[1], "HLE");
  strcpy(Config.Bios[2], "HLE");
  spu_config.iUseThread = 0;
#ifdef PCSX_SYSTEM12
  if (board) {
    for (i = 0; i < (int)(sizeof(boards) / sizeof(boards[0])); i++)
      if (boards[i].family == board_family) selected_board = &boards[i];
    if (!selected_board || (audio_path && !selected_board->open_audio) ||
        (nvram_path && !selected_board->import_nvram)) {
      fprintf(stderr, "Selected board does not provide requested audio/NVRAM output\n");
      if (trace) fclose(trace);
      return 2;
    }
    if (selected_board->load(board)) { if (trace) fclose(trace); return 1; }
  }
  if (nvram_path && load_nvram(nvram_path)) {
    selected_board->unload();
    if (trace) fclose(trace);
    return 1;
  }
  if (inputs_path && load_input_events(inputs_path, &input_events, &input_event_count)) {
    selected_board->unload(); return 2;
  }
#else
  if (board) { fprintf(stderr, "Rebuild with BOARD=1 for arcade boards.\n"); return 2; }
#endif
  if (gpu_type1_test && (gpu_set_model(1) || gpu_set_vram_height(gpu_type1_test == 2 ? 512 : 1024))) {
    fprintf(stderr, "Rebuild with BOARD=1 for type-1 GPU testing.\n"); return 2;
  }
#ifdef PCSX_SYSTEM12_SOURCE_TIMING
  if (system12_active()) Config.cycle_multiplier = 200;
#endif
  if (psxInit() || LoadPlugins()) return 1;
  pl_rearmed_cbs.pl_vout_open = video_open;
  pl_rearmed_cbs.pl_vout_close = video_close;
  pl_rearmed_cbs.pl_vout_set_mode = video_mode;
  pl_rearmed_cbs.pl_vout_flip = video_flip;
  pl_rearmed_cbs.mmap = gpu_alloc;
  pl_rearmed_cbs.munmap = gpu_free;
  pl_rearmed_cbs.gpu_frame_count = &frame_counter;
  pl_rearmed_cbs.gpu_hcnt = &hSyncCount;
  pl_rearmed_cbs.gpu_state_change = gpu_state_change;
  GPUrearmedCallbacks(&pl_rearmed_cbs);
  SPU_registerCallback(SPUirq);
  SPU_registerScheduleCb(SPUschedule);
  if (SPU_open() || GPU_open(NULL, NULL, NULL)) return 1;
  psxReset();
#ifdef PCSX_SYSTEM12
  if (gpu_type1_test) {
    extern int harness_gpu_type1_selftest(void);
    result = harness_gpu_type1_selftest();
    GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
    return result ? 1 : 0;
  }
  if (board_test) {
    extern int harness_gpu_vram_selftest(void);
    extern int harness_gpu_type1_selftest(void);
    if (selected_board->selftest() || (gpu_get_model() == 1 ? harness_gpu_type1_selftest() :
        harness_gpu_vram_selftest())) result = 1;
  }
#endif
  if (!result && exe && Load(exe)) result = 1;
  signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
  start_frame = frame_counter; prev_cycle = psxRegs.cycle;
#ifdef PCSX_SYSTEM12
  if (board && audio_test_fixture) system12_audio_test_fixture();
  if (board && audio_path && selected_board->open_audio(audio_path)) {
    fprintf(stderr, "cannot open WAV output: %s\n", audio_path);
    result = 1;
  }
  if (board && !result && apply_input_events(input_events, input_event_count,
      &next_input_event, 0)) result = 1;
#endif
#ifdef PCSX_SYSTEM12
  if (bus_trace_bins)
    system12_set_bus_trace(harness_system12_bus_event, bus_trace_bins);
#endif
  cycle_limit = (uint64_t)PSXCLK * frames / 30 + PSXCLK;
  clock_gettime(CLOCK_MONOTONIC, &begin);
  while (!result && !interrupted && frame_counter - start_frame < frames) {
    unsigned int steps = 0;
#ifdef PCSX_SYSTEM12
    if (board && apply_input_events(input_events, input_event_count,
        &next_input_event, frame_counter - start_frame)) { result = 1; break; }
#endif
    if (trace && blocks < trace_blocks) {
      fprintf(trace, "%lu,%" PRIu64 ",%08x,%08x", blocks, cycles,
        psxRegs.pc, intFakeFetch(psxRegs.pc));
      for (i = 0; i < 32; i++) fprintf(trace, ",%08x", psxRegs.GPR.r[i]);
      fputc('\n', trace);
    }
    /* Use the public interpreter step to cap otherwise branchless blocks.
     * It preserves the core's branch and pending load-delay semantics. */
    if (use_drc)
      psxCpu->ExecuteBlock(&psxRegs, EXEC_CALLER_BOOT);
    else {
      psxRegs.branchSeen = 0;
      do {
        execI(&psxRegs);
        steps++;
      } while (!psxRegs.branchSeen && steps < 4096 && !interrupted);
    }
    blocks++;
    cycles += (uint32_t)(psxRegs.cycle - prev_cycle);
    prev_cycle = psxRegs.cycle;
#ifdef PCSX_SYSTEM12
    /* Stop permanently before a later 32-bit CPU cycle wrap could reuse the
     * early bins. The harness's elapsed cycle count is monotonic. */
    if (bus_trace_bins && !bus_trace_finished &&
        cycles >= (uint64_t)BOARD_BUS_TRACE_BIN_CYCLES * BOARD_BUS_TRACE_MAX_BINS) {
      system12_set_bus_trace(NULL, NULL);
      bus_trace_finished = 1;
    }
    if (board) {
      int sync_result = selected_board->sync(psxRegs.cycle);
      if (sync_result < 0) {
        fprintf(stderr, "%s: device or bus fault during execution\n", selected_board->name);
        result = 1;
      }
      else if (sync_result > 0) {
        /* A board watchdog requests reset at a safe CPU boundary. Preserve
         * run progress when the core restarts its frame and cycle counters. */
        unsigned int elapsed_frames = frame_counter - start_frame;
        psxReset();
        start_frame = frame_counter - elapsed_frames;
        prev_cycle = psxRegs.cycle;
      }
    }
#endif
    if (snapshots && snapshot_count < 200000 && cycles >= next_snapshot) {
      unsigned int r;
      fprintf(snapshots, "%" PRIu64 ",%u,%08x,%08x,%08x,%08x,%08x,%04x,%04x,%08x"
        ",%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x"
        ",%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
        cycles, frame_counter - start_frame, psxRegs.pc, psxRegs.code,
        psxRegs.CP0.n.SR, psxRegs.CP0.n.Cause, psxRegs.interrupt,
        psxHu16(0x1070), psxHu16(0x1074), psxMemRead32(&psxRegs, 0x80000080),
        psxMemRead32(&psxRegs, 0x800d2190), psxMemRead32(&psxRegs, 0x800d2194),
        psxMemRead32(&psxRegs, 0x800d2198), psxMemRead32(&psxRegs, 0x800d219c),
        psxMemRead32(&psxRegs, 0x800d21a0), psxMemRead32(&psxRegs, 0x800d21a4),
        psxMemRead32(&psxRegs, 0x800d21a8), psxMemRead32(&psxRegs, 0x800d21ac),
        psxMemRead32(&psxRegs, 0x800d21b0), psxMemRead32(&psxRegs, 0x800d21b4),
        psxMemRead32(&psxRegs, 0x800d21b8), psxMemRead32(&psxRegs, 0x800d21bc),
        psxMemRead32(&psxRegs, 0x800d21c0), psxMemRead32(&psxRegs, 0x800d21c4),
        psxMemRead32(&psxRegs, 0x800d21c8), psxMemRead32(&psxRegs, 0x800d21cc),
        psxMemRead32(&psxRegs, 0x800d21d0), psxMemRead32(&psxRegs, 0x800d21d4),
        psxMemRead32(&psxRegs, 0x800d21d8), psxMemRead32(&psxRegs, 0x800d21dc));
      for (r = 0; r < 32; r++) fprintf(snapshots, ",%08x", psxRegs.GPR.r[r]);
      fputc('\n', snapshots);
      snapshot_count++;
      do {
        if (UINT64_MAX - next_snapshot < snapshot_interval) {
          next_snapshot = UINT64_MAX;
          break;
        }
        next_snapshot += snapshot_interval;
      } while (next_snapshot <= cycles);
    }
    /* Bound a malformed boot that changes timer registers or loses vsync. */
    if (cycles > cycle_limit || blocks >= max_blocks) {
      fprintf(stderr, "execution limit reached before requested vsync count\n");
      result = 1; break;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
#ifdef PCSX_SYSTEM12
  if (board && audio_path && selected_board->close_audio()) {
    fprintf(stderr, "failed to finalize WAV output: %s\n", audio_path);
    result = 1;
  }
  free(input_events);
#endif
  if (trace && fclose(trace)) result = 1;
  if (snapshots && fclose(snapshots)) result = 1;
#ifdef PCSX_SYSTEM12
  if (bus_trace_bins) {
    system12_set_bus_trace(NULL, NULL);
    if (write_system12_bus_trace(bus_trace_file, bus_trace_bins)) result = 1;
    if (fclose(bus_trace_file)) result = 1;
    free(bus_trace_bins);
  }
#endif
  if (harness_write_ppm(output, 0, 0)) result = 1;
  if (verify) {
    unsigned short center = harness_vram_pixel(160, 100);
    unsigned short corner = harness_vram_pixel(10, 10);
    if (center != 0x03ff || corner != 0x2082 || psxMemRead32(&psxRegs, 0x8000f000) != 0x534d4f4b) {
      fprintf(stderr, "smoke FAILED: triangle=%04x background=%04x sentinel=%08x\n",
        center, corner, psxMemRead32(&psxRegs, 0x8000f000)); result = 1;
    }
    else fprintf(stderr, "smoke PASS: MIPS MMIO -> GPU_NEON triangle + RAM sentinel\n");
  }
#ifdef PCSX_SYSTEM12
  if (!result && !interrupted && nvram_path && save_nvram(nvram_path)) result = 1;
#endif
  fprintf(stderr, "host=macOS-arm64 renderer=GPU_NEON SIMD cpu=%s "
    "frames=%u cycles=%" PRIu64 " blocks=%lu pc=%08x flips=%u elapsed=%.3fs output=%s\n",
#ifndef DRC_DISABLE
    psxCpu == &psxRec ? "ari64" : "interpreter",
#else
    "interpreter",
#endif
    frame_counter - start_frame, cycles, blocks, psxRegs.pc,
    flips, end.tv_sec - begin.tv_sec + (end.tv_nsec - begin.tv_nsec) * 1e-9, output);
#ifdef PCSX_SYSTEM12
  if (board) selected_board->report(stderr);
#endif
  GPU_close(); SPU_close(); psxShutdown(); ReleasePlugins();
#ifdef PCSX_SYSTEM12
  if (board) selected_board->unload();
#endif
  return result || interrupted ? 1 : 0;
}
