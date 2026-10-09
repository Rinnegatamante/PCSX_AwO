/*
 * Headless macOS libretro host for System 12 measurements and functional runs.
 *
 * Loads the real libretro core (frontend/libretro.c, the code the Vita runs),
 * with the Vita standalone core options and replay semantics: one CSV row
 * set is applied per input poll. There is no display, vsync or audio pacing,
 * so elapsed time is pure emulation work.
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <libretro.h>
#include "../../frontend/vita/input_csv_reader.h"

#define MAX_EVENTS 4096
#define MAX_OPTIONS 64

struct core {
    void *handle;
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*system12_report)(FILE *);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    void **psx_cpu;
    void *psx_rec;
};

static struct core core;
static const char *system_dir = ".", *save_dir = ".";
static struct { char key[64], value[64]; } overrides[MAX_OPTIONS];
static unsigned override_count;
static struct { unsigned frame, port, id, pressed; } events[MAX_EVENTS];
static unsigned event_count, next_event, input_frame;
static uint16_t joypad[2];
static bool replay_active;
static uint16_t *last_frame;
static unsigned last_w, last_h;
static size_t last_capacity;
static uint64_t audio_frames;
static FILE *wav;
static uint64_t wav_bytes;

static void host_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("S12_HOST_VERBOSE") != NULL;
    if (level < RETRO_LOG_WARN && !verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Same defaults as frontend/vita/standalone.c; --option overrides them. */
static const char *option_value(const char *key)
{
    static const struct { const char *key, *value; } defaults[] = {
        { "pcsx_rearmed_memcard1", "shared" },
        { "pcsx_rearmed_memcard2", "none" },
        { "pcsx_rearmed_bios", "auto" },
        { "pcsx_rearmed_show_bios_bootlogo", "disabled" },
        { "pcsx_rearmed_rgb32_output", "disabled" },
        { "pcsx_rearmed_frameskip_type", "disabled" },
        { "pcsx_rearmed_region", "auto" },
        { "pcsx_rearmed_display_info", "disabled" },
        { "pcsx_rearmed_vibration", "disabled" },
        { "pcsx_rearmed_dithering", "enabled" },
        { "pcsx_rearmed_neon_interlace_enable_v2", "auto" },
        { "pcsx_rearmed_neon_enhancement_enable", "disabled" },
        { "pcsx_rearmed_scale_hires", "disabled" },
        { "pcsx_rearmed_gpu_thread_rendering", "disabled" },
        { "pcsx_rearmed_spu_interpolation", "gaussian" },
        { "pcsx_rearmed_spu_thread", "disabled" },
        { "pcsx_rearmed_cd_readahead", "0" },
        { "pcsx_rearmed_drc", "enabled" },
        { "pcsx_rearmed_psxclock", "auto" },
        { "pcsx_rearmed_fractional_framerate", "auto" },
    };
    unsigned i;
    for (i = 0; i < override_count; i++)
        if (!strcmp(overrides[i].key, key)) return overrides[i].value;
    for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        if (!strcmp(defaults[i].key, key)) return defaults[i].value;
    return NULL;
}

static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = host_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;
        var->value = option_value(var->key);
        return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    size_t need = (size_t)width * height;
    unsigned y;
    if (!data || !width || !height) return; /* duplicated frame */
    if (need > last_capacity) {
        uint16_t *grown = realloc(last_frame, need * sizeof(*last_frame));
        if (!grown) return;
        last_frame = grown;
        last_capacity = need;
    }
    for (y = 0; y < height; y++)
        memcpy(last_frame + (size_t)y * width, (const uint8_t *)data + y * pitch,
               width * sizeof(uint16_t));
    last_w = width;
    last_h = height;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    audio_frames += frames;
    if (wav && data && fwrite(data, 4, frames, wav) == frames) wav_bytes += frames * 4;
    return frames;
}

static void audio_sample_cb(int16_t left, int16_t right)
{
    int16_t pair[2] = { left, right };
    audio_batch_cb(pair, 1);
}

/* Bindings of frontend/libretro.c's System 12 inputs (and the Vita replay). */
static int control_binding(const char *name, unsigned *port, unsigned *id)
{
    static const struct { const char *name; unsigned port, id; } bindings[] = {
        { "p1up", 0, RETRO_DEVICE_ID_JOYPAD_UP }, { "p1down", 0, RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "p1left", 0, RETRO_DEVICE_ID_JOYPAD_LEFT }, { "p1right", 0, RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { "p1button1", 0, RETRO_DEVICE_ID_JOYPAD_B }, { "p1button2", 0, RETRO_DEVICE_ID_JOYPAD_A },
        { "p1button3", 0, RETRO_DEVICE_ID_JOYPAD_Y }, { "p1button4", 0, RETRO_DEVICE_ID_JOYPAD_X },
        { "p1button5", 0, RETRO_DEVICE_ID_JOYPAD_L }, { "p1button6", 0, RETRO_DEVICE_ID_JOYPAD_R },
        { "p1button7", 0, RETRO_DEVICE_ID_JOYPAD_L2 }, { "p1button8", 0, RETRO_DEVICE_ID_JOYPAD_R2 },
        { "p1start", 0, RETRO_DEVICE_ID_JOYPAD_START },
        { "p2up", 1, RETRO_DEVICE_ID_JOYPAD_UP }, { "p2down", 1, RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "p2left", 1, RETRO_DEVICE_ID_JOYPAD_LEFT }, { "p2right", 1, RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { "p2button1", 1, RETRO_DEVICE_ID_JOYPAD_B }, { "p2button2", 1, RETRO_DEVICE_ID_JOYPAD_A },
        { "p2button3", 1, RETRO_DEVICE_ID_JOYPAD_Y }, { "p2button4", 1, RETRO_DEVICE_ID_JOYPAD_X },
        { "p2button5", 1, RETRO_DEVICE_ID_JOYPAD_L }, { "p2button6", 1, RETRO_DEVICE_ID_JOYPAD_R },
        { "p2button7", 1, RETRO_DEVICE_ID_JOYPAD_L2 }, { "p2button8", 1, RETRO_DEVICE_ID_JOYPAD_R2 },
        { "p2start", 1, RETRO_DEVICE_ID_JOYPAD_START },
        { "coin1", 0, RETRO_DEVICE_ID_JOYPAD_L3 }, { "coin2", 1, RETRO_DEVICE_ID_JOYPAD_L3 },
        { "service", 0, RETRO_DEVICE_ID_JOYPAD_SELECT }, { "test", 1, RETRO_DEVICE_ID_JOYPAD_SELECT },
    };
    unsigned i;
    for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++)
        if (!strcmp(name, bindings[i].name)) {
            *port = bindings[i].port;
            *id = bindings[i].id;
            return 0;
        }
    return -1;
}

static int load_inputs(const char *path, unsigned frames)
{
    FILE *file = fopen(path, "rb");
    char line[256];
    size_t length, raw;
    unsigned prev_frame = 0;
    int result, have_header = 0;
    if (!file) { perror(path); return -1; }
    while ((result = vita_read_input_line(file, line, sizeof(line), &length, &raw)) > 0) {
        char control[32];
        unsigned frame, pressed, port, id;
        if (length && line[length - 1] == '\r') line[--length] = '\0';
        if (!length || line[0] == '#') continue;
        if (!have_header) {
            if (strcmp(line, "frame,control,pressed")) break;
            have_header = 1;
            continue;
        }
        if (sscanf(line, "%u,%31[^,],%u", &frame, control, &pressed) != 3 || pressed > 1 ||
            frame >= frames || frame < prev_frame || event_count >= MAX_EVENTS ||
            control_binding(control, &port, &id)) {
            result = -1;
            break;
        }
        events[event_count].frame = frame;
        events[event_count].port = port;
        events[event_count].id = id;
        events[event_count].pressed = pressed;
        event_count++;
        prev_frame = frame;
    }
    fclose(file);
    if (result < 0 || !have_header || !event_count) {
        fprintf(stderr, "invalid inputs CSV %s\n", path);
        return -1;
    }
    replay_active = true;
    return 0;
}

static void input_poll_cb(void)
{
    if (!replay_active) return;
    while (next_event < event_count && events[next_event].frame <= input_frame) {
        uint16_t mask = (uint16_t)(1u << events[next_event].id);
        if (events[next_event].pressed) joypad[events[next_event].port] |= mask;
        else joypad[events[next_event].port] &= (uint16_t)~mask;
        next_event++;
    }
    input_frame++;
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || port > 1) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)joypad[port];
    return id <= RETRO_DEVICE_ID_JOYPAD_R3 && (joypad[port] & (1u << id)) != 0;
}

static void *symbol(const char *name, int required)
{
    void *p = dlsym(core.handle, name);
    if (!p && required) {
        fprintf(stderr, "core lacks %s\n", name);
        exit(3);
    }
    return p;
}

static int write_ppm(const char *path)
{
    FILE *out;
    size_t i;
    if (!last_frame) return -1;
    out = fopen(path, "wb");
    if (!out) return -1;
    fprintf(out, "P6\n%u %u\n255\n", last_w, last_h);
    for (i = 0; i < (size_t)last_w * last_h; i++) {
        uint16_t p = last_frame[i];
        uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) << 3), (uint8_t)(((p >> 5) & 63) << 2),
                           (uint8_t)((p & 31) << 3) };
        fwrite(rgb, 1, 3, out);
    }
    return fclose(out);
}

static void put_le(FILE *out, uint32_t value, unsigned bytes)
{
    while (bytes--) { fputc(value & 255, out); value >>= 8; }
}

static void wav_header(FILE *out, unsigned rate, uint32_t data_bytes)
{
    fwrite("RIFF", 1, 4, out); put_le(out, 36 + data_bytes, 4);
    fwrite("WAVEfmt ", 1, 8, out); put_le(out, 16, 4); put_le(out, 1, 2); put_le(out, 2, 2);
    put_le(out, rate, 4); put_le(out, rate * 4, 4); put_le(out, 4, 2); put_le(out, 16, 2);
    fwrite("data", 1, 4, out); put_le(out, data_bytes, 4);
}

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* FNV-1a over main RAM in 64-bit words. The first 64 KiB hold the BIOS
 * kernel, where exception handlers save registers the dynarec may leave
 * stale when they are dead; their values differ between backends without
 * any effect, so they are left out. */
static uint64_t ram_hash(void)
{
    const uint8_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM), i;
    uint64_t hash = 1469598103934665603ull, word;
    for (i = 0x10000; ram && i + 8 <= size; i += 8) {
        memcpy(&word, ram + i, 8);
        hash = (hash ^ word) * 1099511628211ull;
    }
    return hash;
}

/* FNV-1a over the last presented RGB565 frame and its size. */
static uint64_t video_hash(void)
{
    uint64_t hash = 1469598103934665603ull;
    size_t i, n = (size_t)last_w * last_h;
    hash = (hash ^ last_w) * 1099511628211ull;
    hash = (hash ^ last_h) * 1099511628211ull;
    for (i = 0; last_frame && i < n; i++)
        hash = (hash ^ last_frame[i]) * 1099511628211ull;
    return hash;
}

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s --core CORE.dylib --system12 DIR --frames N [--inputs CSV] "
        "[--capture-ppm FILE] [--report-system12 FILE] [--wav FILE] "
        "[--hash-ram FILE] [--hash-video FILE] [--system-dir DIR] [--save-dir DIR] [--option KEY=VALUE]...\n", name);
}

int main(int argc, char **argv)
{
    const char *core_path = NULL, *game = NULL, *inputs = NULL, *ppm = NULL;
    const char *report = NULL, *wav_path = NULL, *hash_path = NULL;
    const char *video_hash_path = NULL;
    FILE *hash_out = NULL, *video_hash_out = NULL;
    unsigned long frames = 0, run;
    struct retro_game_info info = {0};
    struct retro_system_av_info av = {0};
    double started, elapsed;
    int i, result = 0;

    for (i = 1; i < argc; i++) {
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--core") && next) core_path = argv[++i];
        else if (!strcmp(argv[i], "--system12") && next) game = argv[++i];
        else if (!strcmp(argv[i], "--frames") && next) frames = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--inputs") && next) inputs = argv[++i];
        else if (!strcmp(argv[i], "--capture-ppm") && next) ppm = argv[++i];
        else if (!strcmp(argv[i], "--report-system12") && next) report = argv[++i];
        else if (!strcmp(argv[i], "--wav") && next) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--hash-ram") && next) hash_path = argv[++i];
        else if (!strcmp(argv[i], "--hash-video") && next) video_hash_path = argv[++i];
        else if (!strcmp(argv[i], "--system-dir") && next) system_dir = argv[++i];
        else if (!strcmp(argv[i], "--save-dir") && next) save_dir = argv[++i];
        else if (!strcmp(argv[i], "--option") && next && override_count < MAX_OPTIONS) {
            const char *eq = strchr(next, '=');
            if (!eq || eq - next >= 64 || strlen(eq + 1) >= 64) { usage(argv[0]); return 2; }
            memcpy(overrides[override_count].key, next, eq - next);
            overrides[override_count].key[eq - next] = '\0';
            strcpy(overrides[override_count].value, eq + 1);
            override_count++;
            i++;
        }
        else { usage(argv[0]); return 2; }
    }
    if (!core_path || !game || !frames) { usage(argv[0]); return 2; }
    if (inputs && load_inputs(inputs, (unsigned)frames)) return 2;

    core.handle = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!core.handle) { fprintf(stderr, "%s\n", dlerror()); return 3; }
    core.set_environment = symbol("retro_set_environment", 1);
    core.set_video_refresh = symbol("retro_set_video_refresh", 1);
    core.set_audio_sample = symbol("retro_set_audio_sample", 1);
    core.set_audio_sample_batch = symbol("retro_set_audio_sample_batch", 1);
    core.set_input_poll = symbol("retro_set_input_poll", 1);
    core.set_input_state = symbol("retro_set_input_state", 1);
    core.init = symbol("retro_init", 1);
    core.deinit = symbol("retro_deinit", 1);
    core.load_game = symbol("retro_load_game", 1);
    core.unload_game = symbol("retro_unload_game", 1);
    core.run = symbol("retro_run", 1);
    core.get_system_av_info = symbol("retro_get_system_av_info", 1);
    core.system12_report = symbol("system12_report", 0);
    core.psx_cpu = symbol("psxCpu", 0);
    core.psx_rec = symbol("psxRec", 0);
    core.get_memory_data = symbol("retro_get_memory_data", 1);
    core.get_memory_size = symbol("retro_get_memory_size", 1);

    core.set_environment(environment_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    core.init();
    info.path = game;
    if (!core.load_game(&info)) {
        fprintf(stderr, "retro_load_game failed for %s\n", game);
        core.deinit();
        return 3;
    }
    core.get_system_av_info(&av);
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (!wav) { perror(wav_path); return 3; }
        wav_header(wav, (unsigned)av.timing.sample_rate, 0);
    }

    if (hash_path && !(hash_out = fopen(hash_path, "w"))) { perror(hash_path); return 3; }
    if (video_hash_path && !(video_hash_out = fopen(video_hash_path, "w"))) {
        perror(video_hash_path); return 3;
    }

    started = now_seconds();
    for (run = 0; run < frames; run++) {
        core.run();
        if (hash_out)
            fprintf(hash_out, "%lu %016llx\n", run + 1, (unsigned long long)ram_hash());
        if (video_hash_out)
            fprintf(video_hash_out, "%lu %ux%u %016llx\n", run + 1, last_w, last_h,
                (unsigned long long)video_hash());
    }
    elapsed = now_seconds() - started;

    fprintf(stderr, "libretro-host: cpu=%s frames=%lu elapsed=%.3fs fps=%.2f "
        "audio_frames=%llu rate=%.1f\n",
        !core.psx_cpu || !core.psx_rec ? "unknown" :
        *core.psx_cpu == core.psx_rec ? "ari64" : "interpreter",
        frames, elapsed, frames / elapsed, (unsigned long long)audio_frames,
        av.timing.sample_rate);
    if (replay_active) {
        int complete = next_event == event_count;
        fprintf(stderr, "libretro-host: inputs replay %s events=%u/%u final=%04x/%04x\n",
            complete ? "PASS" : "FAIL", next_event, event_count, joypad[0], joypad[1]);
        if (!complete) result = 4;
    }
    if (core.system12_report) core.system12_report(stderr);
    if (report && core.system12_report) {
        FILE *out = fopen(report, "wb");
        if (!out) result = 3;
        else { core.system12_report(out); if (fclose(out)) result = 3; }
    }
    if (ppm && write_ppm(ppm)) { fprintf(stderr, "cannot write %s\n", ppm); result = 3; }
    if (hash_out && fclose(hash_out)) result = 3;
    if (video_hash_out && fclose(video_hash_out)) result = 3;
    if (wav) {
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, (unsigned)av.timing.sample_rate, (uint32_t)wav_bytes);
        if (fclose(wav)) result = 3;
    }
    core.unload_game();
    core.deinit();
    free(last_frame);
    return result;
}
