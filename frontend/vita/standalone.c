#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <SDL3/SDL.h>
#include <libretro.h>
#include "validation.h"

#define APP_ROOT "ux0:data/pcsx-rearmed"
#define BIOS_DIR APP_ROOT "/bios"
#define SAVE_DIR APP_ROOT "/saves"

#define DISPLAY_W 960
#define DISPLAY_H 544
#define DISPLAY_PITCH 960
#define DISPLAY_MEM_SIZE (2 * 1024 * 1024)
#define GAME_W 725
#define GAME_X ((DISPLAY_W - GAME_W) / 2)

#define AUDIO_DEVICE_FRAMES 1024
#define AUDIO_QUEUE_TARGET_FRAMES 2048
#define AUDIO_FRAME_BYTES (sizeof(int16_t) * 2)
#define AUDIO_QUEUE_TARGET_BYTES (AUDIO_QUEUE_TARGET_FRAMES * AUDIO_FRAME_BYTES)

static SceUID g_fb_uid[2] = { -1, -1 };
static uint32_t *g_fb[2];
static int g_front;
static int g_map_w = -1;
static int g_map_h = -1;
static uint16_t g_xmap[GAME_W];
static uint16_t g_ymap[DISPLAY_H];

static SceCtrlData g_pad;
static int g_exit_hold;
static bool g_exit_requested;

static SDL_AudioStream *g_audio_stream;
static uint32_t g_audio_put_errors;

static SceUID g_vm_block = -1;
extern int _newlib_vm_size_user;

int getVMBlock(void)
{
    int size;

    if (g_vm_block > 0)
        return g_vm_block;

    size = (_newlib_vm_size_user + 0xfffff) & ~0xfffff;
    g_vm_block = sceKernelAllocMemBlockForVM("pcsx-jit", size);
    sceClibPrintf("pcsx-vita: JIT VM block size=0x%x uid=0x%x\n", size, g_vm_block);
    return g_vm_block;
}

static void vita_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    (void)level;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sceClibPrintf("%s", buf);
}

static const char *option_value(const char *key)
{
    static const struct {
        const char *key;
        const char *value;
    } options[] = {
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
    size_t i;

    for (i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (strcmp(options[i].key, key) == 0)
            return options[i].value;
    }
    return NULL;
}

static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = vita_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = BIOS_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = SAVE_DIR;
        return true;
#ifdef RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = APP_ROOT;
        return true;
#endif
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        var->value = option_value(var->key);
        return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned *)data = 0;
        return true;
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

static uint32_t rgb565_to_abgr8888(uint16_t p)
{
    uint32_t r5 = (p >> 11) & 31;
    uint32_t g6 = (p >> 5) & 63;
    uint32_t b5 = p & 31;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g6 << 2) | (g6 >> 4);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

static void rebuild_scale_map(unsigned width, unsigned height)
{
    int x, y;
    for (x = 0; x < GAME_W; x++)
        g_xmap[x] = (uint16_t)(((uint64_t)x * width) / GAME_W);
    for (y = 0; y < DISPLAY_H; y++)
        g_ymap[y] = (uint16_t)(((uint64_t)y * height) / DISPLAY_H);
    g_map_w = (int)width;
    g_map_h = (int)height;
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    SceDisplayFrameBuf frame;
    uint32_t *dst;
    int back;
    int y, x;

    if (!data || width == 0 || height == 0)
        return;

    if (g_map_w != (int)width || g_map_h != (int)height)
        rebuild_scale_map(width, height);

    back = g_front ^ 1;
    dst = g_fb[back];

    for (y = 0; y < DISPLAY_H; y++) {
        const uint16_t *src = (const uint16_t *)((const uint8_t *)data + (size_t)g_ymap[y] * pitch);
        uint32_t *out = dst + y * DISPLAY_PITCH + GAME_X;
        for (x = 0; x < GAME_W; x++)
            out[x] = rgb565_to_abgr8888(src[g_xmap[x]]);
    }

    frame.size = sizeof(frame);
    frame.base = dst;
    frame.pitch = DISPLAY_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = DISPLAY_W;
    frame.height = DISPLAY_H;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    g_front = back;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{

    if (!g_audio_stream || !data || frames == 0)
        return frames;

    if (!SDL_PutAudioStreamData(g_audio_stream, data, (int)(frames * AUDIO_FRAME_BYTES))) {
        if (g_audio_put_errors < 8u)
            sceClibPrintf("pcsx-vita: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        g_audio_put_errors++;
        return frames;
    }

    while (!g_exit_requested &&
           SDL_GetAudioStreamQueued(g_audio_stream) > (int)AUDIO_QUEUE_TARGET_BYTES)
        sceKernelDelayThread(1000);

    return frames;
}

static void input_poll_cb(void)
{
    const uint32_t quit_mask = SCE_CTRL_SELECT | SCE_CTRL_START | SCE_CTRL_L1 | SCE_CTRL_R1;
    memset(&g_pad, 0, sizeof(g_pad));
    if (sceCtrlPeekBufferPositiveExt2(0, &g_pad, 1) < 0)
        sceCtrlPeekBufferPositive(0, &g_pad, 1);

    if ((g_pad.buttons & quit_mask) == quit_mask) {
        if (++g_exit_hold >= 120)
            g_exit_requested = true;
    } else {
        g_exit_hold = 0;
    }
}

static int16_t analog_value(uint8_t v)
{
    return (int16_t)(((int)v - 128) * 256);
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    uint32_t button = 0;

    if (port != 0)
        return 0;

    device &= RETRO_DEVICE_MASK;
    if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? analog_value(g_pad.lx) : analog_value(g_pad.ly);
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? analog_value(g_pad.rx) : analog_value(g_pad.ry);
        return 0;
    }
    if (device != RETRO_DEVICE_JOYPAD)
        return 0;

    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:      button = SCE_CTRL_CROSS; break;
    case RETRO_DEVICE_ID_JOYPAD_A:      button = SCE_CTRL_CIRCLE; break;
    case RETRO_DEVICE_ID_JOYPAD_Y:      button = SCE_CTRL_SQUARE; break;
    case RETRO_DEVICE_ID_JOYPAD_X:      button = SCE_CTRL_TRIANGLE; break;
    case RETRO_DEVICE_ID_JOYPAD_L:      button = SCE_CTRL_L1; break;
    case RETRO_DEVICE_ID_JOYPAD_R:      button = SCE_CTRL_R1; break;
    case RETRO_DEVICE_ID_JOYPAD_L2:     button = SCE_CTRL_L2; break;
    case RETRO_DEVICE_ID_JOYPAD_R2:     button = SCE_CTRL_R2; break;
    case RETRO_DEVICE_ID_JOYPAD_L3:     button = SCE_CTRL_L3; break;
    case RETRO_DEVICE_ID_JOYPAD_R3:     button = SCE_CTRL_R3; break;
    case RETRO_DEVICE_ID_JOYPAD_UP:     button = SCE_CTRL_UP; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   button = SCE_CTRL_DOWN; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   button = SCE_CTRL_LEFT; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  button = SCE_CTRL_RIGHT; break;
    case RETRO_DEVICE_ID_JOYPAD_START:  button = SCE_CTRL_START; break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: button = SCE_CTRL_SELECT; break;
    default: return 0;
    }
    return (g_pad.buttons & button) != 0;
}

static int display_init(void)
{
    SceDisplayFrameBuf frame;
    int i;

    for (i = 0; i < 2; i++) {
        g_fb_uid[i] = sceKernelAllocMemBlock(i ? "pcsx-fb1" : "pcsx-fb0",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DISPLAY_MEM_SIZE, NULL);
        if (g_fb_uid[i] < 0)
            return g_fb_uid[i];
        if (sceKernelGetMemBlockBase(g_fb_uid[i], (void **)&g_fb[i]) < 0)
            return -1;
        memset(g_fb[i], 0, DISPLAY_MEM_SIZE);
    }

    frame.size = sizeof(frame);
    frame.base = g_fb[0];
    frame.pitch = DISPLAY_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = DISPLAY_W;
    frame.height = DISPLAY_H;
    g_front = 0;
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void display_finish(void)
{
    int i;
    /* The display retains a framebuffer after the last video callback. Stop
     * scanout and wait for that update before releasing its backing blocks. */
    int ret = sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (ret >= 0)
        ret = sceDisplayWaitSetFrameBuf();
    if (ret < 0) {
        sceClibPrintf("pcsx-vita: display detach failed 0x%x; retaining buffers\n", ret);
        return;
    }
    for (i = 0; i < 2; i++) {
        if (g_fb_uid[i] >= 0)
            sceKernelFreeMemBlock(g_fb_uid[i]);
        g_fb_uid[i] = -1;
        g_fb[i] = NULL;
    }
}

static int audio_init(int sample_rate)
{
    SDL_AudioSpec want;
    SDL_AudioSpec src_spec;
    SDL_AudioSpec dst_spec;
    SDL_AudioSpec device_spec;
    int device_frames = 0;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        sceClibPrintf("pcsx-vita: SDL audio init failed: %s\n", SDL_GetError());
        return -1;
    }

    memset(&want, 0, sizeof(want));
    want.freq = sample_rate;
    want.format = SDL_AUDIO_S16;
    want.channels = 2;

    g_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
    if (!g_audio_stream) {
        sceClibPrintf("pcsx-vita: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    memset(&src_spec, 0, sizeof(src_spec));
    memset(&dst_spec, 0, sizeof(dst_spec));
    memset(&device_spec, 0, sizeof(device_spec));
    SDL_GetAudioStreamFormat(g_audio_stream, &src_spec, &dst_spec);
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_audio_stream),
        &device_spec, &device_frames);
    sceClibPrintf("pcsx-vita: audio stream src=%dHz dst=%dHz device=%dHz frames=%d\n",
        src_spec.freq, dst_spec.freq, device_spec.freq, device_frames);

    if (!SDL_ResumeAudioStreamDevice(g_audio_stream)) {
        sceClibPrintf("pcsx-vita: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    return 0;
}

static void audio_finish(void)
{
    if (g_audio_stream) {
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static bool has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext), i;
    if (nl < el)
        return false;
    name += nl - el;
    for (i = 0; i < el; i++) {
        char a = name[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

static int extension_rank(const char *name)
{
    static const char *exts[] = { ".cue", ".chd", ".pbp", ".m3u", ".iso", ".img", ".bin", ".exe" };
    int i;
    for (i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); i++)
        if (has_ext(name, exts[i]))
            return i;
    return 999;
}

static bool find_game(char *out, size_t out_size)
{
    SceUID dfd;
    SceIoDirent ent;
    char best[256] = {0};
    int best_rank = 999;

    dfd = sceIoDopen(APP_ROOT);
    if (dfd < 0)
        return false;

    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dfd, &ent) > 0) {
        int rank = extension_rank(ent.d_name);
        if (rank < best_rank) {
            snprintf(best, sizeof(best), "%s", ent.d_name);
            best_rank = rank;
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dfd);

    if (best_rank == 999)
        return false;
    snprintf(out, out_size, "%s/%s", APP_ROOT, best);
    return true;
}

static bool is_game_path(const char *path)
{
    const char *base;

    if (path == NULL || path[0] == '\0' || extension_rank(path) == 999)
        return false;

    base = strrchr(path, '/');
    {
        const char *backslash = strrchr(path, '\\');
        if (backslash != NULL && (base == NULL || backslash > base))
            base = backslash;
    }
    base = base != NULL ? base + 1 : path;

    if (strcasecmp(base, "eboot.bin") == 0 ||
        strcasecmp(base, "pcsx-rearmed-vita.elf") == 0 ||
        strcasecmp(base, "pcsx-rearmed-vita.velf") == 0)
        return false;

    return true;
}
static void make_dirs(void)
{
    sceIoMkdir(APP_ROOT, 0777);
    sceIoMkdir(BIOS_DIR, 0777);
    sceIoMkdir(SAVE_DIR, 0777);
}

int main(int argc, char **argv)
{
    struct retro_game_info game;
    char game_path[1024];
    int ret;
    unsigned validation_frames = 0;
    bool verify_smoke = false;
    bool system12_profile = false;
    struct retro_system_av_info av;

    game_path[0] = '\0';
    for (ret = 1; ret < argc; ret++) {
        if (!strcmp(argv[ret], "--verify-smoke")) { verify_smoke = true; continue; }
        if (!strcmp(argv[ret], "--frames")) {
            char *end;
            unsigned long count;
            if (ret + 1 >= argc) return 4;
            count = strtoul(argv[++ret], &end, 10);
            if (*end || !count || count > 216000) return 4;
            validation_frames = (unsigned)count; continue;
        }
        if (!strcmp(argv[ret], "--system12")) {
#ifdef ARCADE_SYSTEM12
            if (ret + 1 >= argc || !argv[ret + 1][0]) return 4;
            system12_profile = true;
            snprintf(game_path, sizeof(game_path), "%s", argv[++ret]);
            continue;
#else
            sceClibPrintf("pcsx-vita: --system12 requires ARCADE_SYSTEM12=1\n");
            return 4;
#endif
        }
        if (!system12_profile && is_game_path(argv[ret]))
            snprintf(game_path, sizeof(game_path), "%s", argv[ret]);
    }
    if (system12_profile && verify_smoke) {
        sceClibPrintf("pcsx-vita: --verify-smoke requires the console smoke executable\n");
        return 4;
    }

    sceClibPrintf("pcsx-vita: main entered\n");
    make_dirs();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    ret = display_init();
    if (ret < 0) {
        sceClibPrintf("pcsx-vita: display init failed 0x%x\n", ret);
        return 1;
    }
    sceClibPrintf("pcsx-vita: display initialized\n");

    sceClibPrintf("pcsx-vita: entering retro_init\n");
    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(NULL);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
    sceClibPrintf("pcsx-vita: retro_init complete\n");

    if (game_path[0] == '\0' && !find_game(game_path, sizeof(game_path))) {
        sceClibPrintf("pcsx-vita: core initialized; no game found in %s\n", APP_ROOT);
        retro_deinit();
        audio_finish();
        display_finish();
        if (g_vm_block > 0)
            sceKernelFreeMemBlock(g_vm_block);
        return 2;
    }

    sceClibPrintf("pcsx-vita: booting %s\n", game_path);

    memset(&game, 0, sizeof(game));
    game.path = game_path;
    if (!retro_load_game(&game)) {
        sceClibPrintf("pcsx-vita: retro_load_game failed\n");
        retro_deinit();
        audio_finish();
        display_finish();
        if (g_vm_block > 0)
            sceKernelFreeMemBlock(g_vm_block);
        return 3;
    }

    retro_get_system_av_info(&av);
    ret = audio_init((int)av.timing.sample_rate);
    if (ret < 0)
        sceClibPrintf("pcsx-vita: audio init failed 0x%x; continuing silent\n", ret);
    else
        sceClibPrintf("pcsx-vita: audio initialized\n");

    unsigned runs = 0;
    sceClibPrintf("pcsx-vita: cpu_driver=%s\n", pcsx_vita_uses_dynarec() ? "ari64" : "interpreter");
    while (!g_exit_requested && (!validation_frames || runs < validation_frames)) {
        retro_run(); ++runs;
    }
    int validation_result = 0;
    if (verify_smoke) {
        pcsx_vita_smoke_result result;
        pcsx_vita_read_smoke(&result);
        validation_result = !pcsx_vita_uses_dynarec() || result.triangle != 0x03ff ||
                            result.background != 0x2082 || result.sentinel != 0x534d4f4b;
        sceClibPrintf("pcsx-vita: smoke %s cpu=%s triangle=%04x background=%04x sentinel=%08x runs=%u\n",
            validation_result ? "FAIL" : "PASS", pcsx_vita_uses_dynarec() ? "ari64" : "interpreter",
            result.triangle, result.background, result.sentinel, runs);
    }

    retro_unload_game();
    retro_deinit();
    audio_finish();
    display_finish();
    if (g_vm_block > 0)
        sceKernelFreeMemBlock(g_vm_block);
    return validation_result ? 5 : 0;
}
