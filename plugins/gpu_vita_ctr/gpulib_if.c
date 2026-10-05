/* PlayStation Vita hardware GPU backend for PCSX-ReARMed. The native renderer is derived from REDRIVER2/PsyCross MIT code through CTR Native / the PSXRecomp Vita integration. Keep the corresponding third-party notices when redistributing this backend. */

#include <SDL3/SDL.h>
#include "platform/native_vita_path.h"
#if defined(__vita__)
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vitaGL.h>
#elif defined(PCSX_NATIVE_VITA_PATH)
#include "platform/mac_vita_compat.h"
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <macros.h>
#include <platform.h>
#include "platform/native_gpu.h"
#include "platform/native_gpu_links.h"
#include "platform/native_renderer.h"

#ifdef force_inline
#undef force_inline
#endif
#include "../gpulib/gpu.h"
#include "../gpulib/gpu_timing.h"
#include "../../frontend/plugin_lib.h"

SDL_Window *g_window;
int gNativeAntiAliasingEnabled;
int gNativeDitheringEnabled = 1;
int g_dbg_emulatorPaused;
int g_dbg_polygonSelected = -1;

/* Set while the backend is stopped; shader/storage geometry stays fixed per context. */
unsigned g_nativeVramHeight = GPU_VRAM_ROWS_CONSOLE;

static int s_context_ready;
static int s_frame_started;
static uint16_t *s_guest_vram;

void Platform_Log(const char *fmt, ...);

extern int ParseTaglessPrimitive(u32 *command);
extern void VitaCtrNativeGpu_SetDisplayEnv(int x, int y, int w, int h, int depth24);
extern void VitaCtrNativeRenderer_SetPresentationAspect(int width, int height);
extern void VitaCtrNativeRenderer_CopyGuestVRAM(const u16 *src, int x, int y, int w, int h);
extern void VitaCtrNativeRenderer_ReadGuestVRAM(u16 *dst, int x, int y, int w, int h);
extern void NativeGpu_SetFrontendDisplayBlank(int blank);
extern int NativeGpu_GetRenderDisplayBlank(void);
extern void NativeGpu_ForceSynchronousFrame(void);
extern void NativeRenderer_PresentBlank(void);

void Platform_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void Platform_LogError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void Platform_LogWarn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void Platform_LogFlush(void)
{
    fflush(stdout);
    fflush(stderr);
}

void Platform_PollHostEvents(void) {}

int NativeAdhoc_IsDialogRunning(void)
{
    return 0;
}

b32 NativeGhostInput_GetReplayOverlayState(u32 *buttonsHeld, u8 *stickLX,
    u8 *stickLY, u8 *stickRX, u8 *stickRY)
{
    (void)buttonsHeld;
    (void)stickLX;
    (void)stickLY;
    (void)stickRX;
    (void)stickRY;
    return 0;
}

int NativeGpuLinks_IsRegisteredHostPointer(const void *hostPtr)
{
    (void)hostPtr;
    return 0;
}

int NativeGpuLinks_IsRegisteredHostRange(const void *hostPtr, size_t size)
{
    (void)hostPtr;
    (void)size;
    return 0;
}

int Platform_BeginScene(void)
{
    NativeRenderer_BeginScene();
    return 1;
}

void Platform_EndScene(void)
{
    const DISPENV *display = NativeGpu_GetRenderDispEnv();
    const DRAWENV *draw = NativeGpu_GetRenderDrawEnv();
    const int blank = NativeGpu_GetRenderDisplayBlank();

    NativeRenderer_EndScene();
    if (blank) {
        if (!display->isrgb24)
            NativeRenderer_StoreFrameBuffer(draw->clip.x, draw->clip.y, draw->clip.w, draw->clip.h);
        NativeRenderer_PresentBlank();
    }
    else if (display->isrgb24) {
        NativeRenderer_PresentVRAMDisplay();
    }
    else {
        NativeRenderer_StoreFrameBuffer(draw->clip.x, draw->clip.y, draw->clip.w, draw->clip.h);
        /* Scanout reads DISPENV, independently of the final GP0 draw page. */
        NativeRenderer_PresentVRAMRect(display->disp.x, display->disp.y, display->disp.w, display->disp.h);
    }
    NativeRenderer_SwapWindow();
}

typedef struct VitaCtrVramTask {
    uint16_t *pixels;
    int x, y, w, h;
    int dst_x, dst_y;
    uint8_t r, g, b;
} VitaCtrVramTask;

static void backend_clear(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    NativeRenderer_ClearVRAM(task->x, task->y, task->w, task->h,
        task->r, task->g, task->b);
}

static void backend_copy(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    NativeRenderer_CopyVRAM(task->pixels, task->x, task->y, task->w, task->h,
        task->dst_x, task->dst_y);
}

static void backend_read(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    NativeRenderer_ReadVRAM(task->pixels, task->x, task->y, task->w, task->h);
}

static void backend_copy_guest(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    VitaCtrNativeRenderer_CopyGuestVRAM(s_guest_vram,
        task->x, task->y, task->w, task->h);
    NativeRenderer_UpdateVRAM();
}

static void backend_read_guest(void *arg)
{
    VitaCtrVramTask *task = (VitaCtrVramTask *)arg;
    NativeRenderer_EndScene();
    VitaCtrNativeRenderer_ReadGuestVRAM(s_guest_vram,
        task->x, task->y, task->w, task->h);
}

static void vram_task_sync(NativeGpuBackendTaskFn callback, VitaCtrVramTask *task)
{
    if (NativeGpu_HasPendingSplits()) {
        NativeGpu_ForceSynchronousFrame();
        NativeGpu_FlushFrontendSplitsSync();
    }
    NativeGpu_RunBackendTaskSync(callback, task);
}

int ClearImage(RECT16 *rect, uint8_t r, uint8_t g, uint8_t b)
{
    VitaCtrVramTask task;
    if (!rect)
        return 0;
    memset(&task, 0, sizeof(task));
    task.x = rect->x;
    task.y = rect->y;
    task.w = rect->w;
    task.h = rect->h;
    task.r = r;
    task.g = g;
    task.b = b;
    vram_task_sync(backend_clear, &task);
    return 0;
}

int LoadImage(RECT16 *rect, void *p)
{
    VitaCtrVramTask task;
    if (!rect || !p)
        return 0;
    memset(&task, 0, sizeof(task));
    task.pixels = (uint16_t *)p;
    task.w = rect->w;
    task.h = rect->h;
    task.dst_x = rect->x;
    task.dst_y = rect->y;
    vram_task_sync(backend_copy, &task);
    return 0;
}

int MoveImage(RECT16 *rect, int x, int y)
{
    VitaCtrVramTask task;
    if (!rect)
        return 0;
    memset(&task, 0, sizeof(task));
    task.x = rect->x;
    task.y = rect->y;
    task.w = rect->w;
    task.h = rect->h;
    task.dst_x = x;
    task.dst_y = y;
    vram_task_sync(backend_copy, &task);
    return 0;
}

int StoreImage(RECT16 *rect, uint32_t *p)
{
    VitaCtrVramTask task;
    if (!rect || !p)
        return 0;
    memset(&task, 0, sizeof(task));
    task.pixels = (uint16_t *)p;
    task.x = rect->x;
    task.y = rect->y;
    task.w = rect->w;
    task.h = rect->h;
    vram_task_sync(backend_read, &task);
    return 0;
}

static void submit_gp0_packet(const uint32_t *words, int count)
{
    uint32_t packet[257];

    if (!s_context_ready || !words || count <= 0 || count > 255)
        return;

    packet[0] = ((uint32_t)count << 24) | 0x00ffffffu;
    memcpy(packet + 1, words, (size_t)count * sizeof(uint32_t));
    ParseTaglessPrimitive((u32 *)(packet + 1));
}

typedef struct RendererInitTask {
    int result;
} RendererInitTask;

static void backend_renderer_init(void *arg)
{
    RendererInitTask *task = (RendererInitTask *)arg;
    task->result = 0;

    if (!NativeRenderer_InitialiseRender((char *)"PCSX-ReARMed", 960, 544, 0))
        return;
    if (!NativeRenderer_InitialisePSX())
        return;

    VitaCtrNativeRenderer_SetPresentationAspect(4, 3);
    VitaCtrNativeRenderer_CopyGuestVRAM(s_guest_vram, 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    NativeRenderer_UpdateVRAM();
    task->result = 1;
}

static void backend_renderer_shutdown(void *arg)
{
    (void)arg;
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace GPU wait begin\n");
#endif
    /* The worker being idle does not mean GXM has finished consuming VAOs
     * and transfer storage. Wait before any of those objects are released. */
    glFinish();
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace renderer delete begin\n");
#endif
    NativeRenderer_Shutdown();
    /* Destroy plugin GL objects, flush their GXM work, then release vitaGL's
     * context and GXM resources before the guest CRT tears down its heap. */
    glFinish();
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace vitaGL shutdown begin\n");
#endif
    int shutdown_result = vglShutdown();
    if (shutdown_result < 0)
        SysPrintf("pcsx-vita: vitaGL shutdown FAIL (%d)\n", shutdown_result);
    else
        SysPrintf("pcsx-vita: vitaGL shutdown PASS (%d)\n", shutdown_result);
}

static int init_context(void)
{
    RendererInitTask task;

    if (s_context_ready)
        return 1;
    if (!s_guest_vram)
        return 0;
    if (gpu_get_model() != 2) {
        SysPrintf("pcsx-vita: VitaGL rejects unimplemented GPU type 1\n");
        return 0;
    }
    g_nativeVramHeight = gpu_get_vram_height();
    if (g_nativeVramHeight != GPU_VRAM_ROWS_CONSOLE &&
        g_nativeVramHeight != GPU_VRAM_ROWS_SYSTEM12)
        return 0;

#ifdef __vita__
    sceIoMkdir("ux0:data/pcsx-rearmed", 0777);
    sceIoMkdir("ux0:data/pcsx-rearmed/shader_cache", 0777);
#endif

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SysPrintf("pcsx-vita: SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!NativeGpu_InitBackend(960, 544)) {
        SysPrintf("pcsx-vita: High Octane GPU thread init failed\n");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }

    memset(&task, 0, sizeof(task));
    NativeGpu_RunBackendTaskSync(backend_renderer_init, &task);
    if (!task.result) {
        SysPrintf("pcsx-vita: High Octane renderer init failed\n");
        /* vitaGL was initialized by the backend thread before renderer setup.
         * Even a partial renderer init must release that context before CRT
         * teardown reclaims the guest heap. The renderer shutdown tolerates
         * zero GL names and clears any objects created before the failure. */
        NativeGpu_RunBackendTaskSync(backend_renderer_shutdown, NULL);
        NativeGpu_ShutdownBackend();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }

    VitaCtrNativeGpu_SetDisplayEnv(0, 0, 320, 240, 0);
    NativeGpu_BeginFrontendFrame();
    s_frame_started = 1;
    s_context_ready = 1;
    SysPrintf("pcsx-vita: GPU = CTR High Octane native, VRAM=%ux%u, 4:3 725x544 target\n",
        (unsigned)VRAM_WIDTH, g_nativeVramHeight);
    return 1;
}

static int command_words(const uint32_t *list, int count, int *cmd_out)
{
    int cmd;
    int len;
    int n;

    if (count <= 0)
        return 0;
    cmd = (int)(LE32TOH(list[0]) >> 24);
    *cmd_out = cmd;
    len = 1 + cmd_lengths[cmd];
    if (len > count)
        return 0;

    if (cmd >= 0x48 && cmd <= 0x4f) {
        for (n = 2; ; n++) {
            if (n + 1 >= count)
                return 0;
            if ((LE32TOH(list[n + 1]) & 0xf000f000u) == 0x50005000u)
                break;
        }
        len += n - 2;
    }
    else if (cmd >= 0x58 && cmd <= 0x5f) {
        for (n = 2; ; n++) {
            if (n * 2 >= count)
                return 0;
            if ((LE32TOH(list[n * 2]) & 0xf000f000u) == 0x50005000u)
                break;
        }
        len += (n - 2) * 2;
    }
    return len;
}

static void account_timing(const uint32_t *list, int cmd,
    uint32_t *cycle_sum, uint32_t *cycle_last)
{
    const int16_t *slist = (const int16_t *)list;
    int w, h;

    switch (cmd) {
    case 0x02:
        w = ((LE16TOH(slist[4]) & 0x3ff) + 0xf) & ~0xf;
        h = LE16TOH(slist[5]) & GPU_VRAM_Y_MASK;
        gput_sum(*cycle_sum, *cycle_last, gput_fill(w, h));
        break;
    case 0x20 ... 0x23: gput_sum(*cycle_sum, *cycle_last, gput_poly_base()); break;
    case 0x24 ... 0x27: gput_sum(*cycle_sum, *cycle_last, gput_poly_base_t()); break;
    case 0x28 ... 0x2b: gput_sum(*cycle_sum, *cycle_last, gput_quad_base()); break;
    case 0x2c ... 0x2f: gput_sum(*cycle_sum, *cycle_last, gput_quad_base_t()); break;
    case 0x30 ... 0x33: gput_sum(*cycle_sum, *cycle_last, gput_poly_base_g()); break;
    case 0x34 ... 0x37: gput_sum(*cycle_sum, *cycle_last, gput_poly_base_gt()); break;
    case 0x38 ... 0x3b: gput_sum(*cycle_sum, *cycle_last, gput_quad_base_g()); break;
    case 0x3c ... 0x3f: gput_sum(*cycle_sum, *cycle_last, gput_quad_base_gt()); break;
    case 0x40 ... 0x4f:
    case 0x50 ... 0x5f:
        gput_sum(*cycle_sum, *cycle_last, gput_line(0));
        break;
    case 0x60 ... 0x63:
        w = LE16TOH(slist[4]) & 0x3ff;
        h = LE16TOH(slist[5]) & GPU_VRAM_Y_MASK;
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(w, h));
        break;
    case 0x64 ... 0x67:
        w = LE16TOH(slist[6]) & 0x3ff;
        h = LE16TOH(slist[7]) & GPU_VRAM_Y_MASK;
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(w, h));
        break;
    case 0x68 ... 0x6b:
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(1, 1));
        break;
    case 0x70 ... 0x77:
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(8, 8));
        break;
    case 0x78 ... 0x7f:
        gput_sum(*cycle_sum, *cycle_last, gput_sprite(16, 16));
        break;
    default:
        break;
    }
}

int renderer_do_cmd_list(uint32_t *list, int count, uint32_t *ex_regs,
    int *cycles_sum, int *cycles_last, int *last_cmd)
{
    uint32_t cyc_sum = 0;
    uint32_t cyc = (uint32_t)*cycles_last;
    int pos = 0;
    int cmd = 0;

    while (pos < count) {
        int len = command_words(list + pos, count - pos, &cmd);
        uint32_t *packet = list + pos;

        if (len <= 0) {
            cmd = -1;
            break;
        }
        if (cmd >= 0x80 && cmd <= 0xdf)
            break;

        account_timing(packet, cmd, &cyc_sum, &cyc);

        if (cmd >= 0x24 && cmd <= 0x3f && (cmd & 4)) {
            const int tpage_index = 4 + ((cmd >> 4) & 1);
            ex_regs[1] = (ex_regs[1] & ~0x1fffu) |
                ((LE32TOH(packet[tpage_index]) >> 16) & 0x1fffu);
        }
        if (cmd >= 0xe0 && cmd <= 0xe7)
            ex_regs[cmd & 7] = LE32TOH(packet[0]);

        submit_gp0_packet(packet, len);
        pos += len;
    }

    *cycles_sum += (int)cyc_sum;
    *cycles_last = (int)cyc;
    *last_cmd = cmd;
    return pos;
}

int renderer_init(void)
{
    /* gpulib maps VRAM later from GPUrearmedCallbacks(). Keep GPUinit cheap and create the vitaGL backend once gpu.vram is available. */
    s_guest_vram = gpu.vram;
    return 0;
}

void renderer_finish(void)
{
    if (!s_context_ready)
        return;

#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace worker sync begin frame_started=%d\n", s_frame_started);
#endif
    NativeGpu_SyncBackend();
#ifdef VITA_CTR_GPU_TRACE
    SysPrintf("pcsx-vita: teardown-trace worker sync done\n");
#endif
    NativeGpu_RunBackendTaskSync(backend_renderer_shutdown, NULL);
    NativeGpu_ShutdownBackend();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    s_context_ready = 0;
    s_frame_started = 0;
    s_guest_vram = NULL;
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
    int i;
    if (!s_context_ready || !ecmds)
        return;
    for (i = 1; i <= 6; i++)
        submit_gp0_packet(&ecmds[i], 1);
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
    VitaCtrVramTask task;
    (void)state_changed;
    if (!s_context_ready || !s_guest_vram || w <= 0 || h <= 0)
        return;

    /* A GP0 upload is ordered after any earlier draws even if the frontend
     * has not submitted its frame packet yet. Flush those splits before the
     * backend copies guest VRAM into the persistent GL texture. This callback
     * runs on the GPU/frontend thread; backend tasks never call it. */
    renderer_barrier();

    memset(&task, 0, sizeof(task));
    task.x = x;
    task.y = y;
    task.w = w;
    task.h = h;
    NativeGpu_RunBackendTaskSync(backend_copy_guest, &task);
}

void renderer_flush_queues(void)
{
    /* Submission happens at vout_update. Keeping this non-blocking preserves the renderer thread overlap that CTR Native relies on. */
}

void renderer_barrier(void)
{
    if (!s_context_ready)
        return;
    if (NativeGpu_HasPendingSplits()) {
        NativeGpu_ForceSynchronousFrame();
        NativeGpu_FlushFrontendSplitsSync();
    }
    NativeGpu_SyncBackend();
}

void renderer_sync_vram_to_cpu(int x, int y, int w, int h)
{
    VitaCtrVramTask task;
    if (!s_context_ready || !s_guest_vram || w <= 0 || h <= 0)
        return;

    renderer_barrier();
    memset(&task, 0, sizeof(task));
    task.x = x;
    task.y = y;
    task.w = w;
    task.h = h;
    NativeGpu_RunBackendTaskSync(backend_read_guest, &task);
}

void renderer_set_interlace(int enable, int is_odd)
{
    (void)enable;
    (void)is_odd;
}

void renderer_set_config(const struct rearmed_cbs *cbs)
{
    s_guest_vram = gpu.vram;
    if (!s_context_ready && !init_context()) {
        SysPrintf("pcsx-vita: deferred High Octane renderer init failed\n");
        return;
    }
    if (cbs->pl_set_gpu_caps)
        cbs->pl_set_gpu_caps(GPU_CAP_OWNS_DISPLAY);
    gNativeDitheringEnabled = cbs->dithering != 0;
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
    (void)screen;
}

int renderer_present_frame(struct psx_gpu *psx_gpu, int src_x, int src_y)
{
    int width;
    int height;

    if (!s_context_ready)
        return 0;

    width = psx_gpu->screen.hres > 0 ? psx_gpu->screen.hres : psx_gpu->screen.w;
    height = psx_gpu->screen.vres > 0 ? psx_gpu->screen.vres : psx_gpu->screen.h;
    if (width <= 0 || height <= 0)
        return 0;

    VitaCtrNativeGpu_SetDisplayEnv(src_x, src_y, width, height,
        !!(psx_gpu->status & PSX_GPU_STATUS_RGB24));
    NativeGpu_SetFrontendDisplayBlank(0);
    if (!s_frame_started) {
        NativeGpu_BeginFrontendFrame();
        s_frame_started = 1;
    }
    NativeGpu_SubmitFrontendFrame();
    NativeGpu_BeginFrontendFrame();
    s_frame_started = 1;
    return 1;
}

void renderer_present_blank(void)
{
    if (!s_context_ready)
        return;

    if (!s_frame_started) {
        NativeGpu_BeginFrontendFrame();
        s_frame_started = 1;
    }
    NativeGpu_SetFrontendDisplayBlank(1);
    NativeGpu_SubmitFrontendFrame();
    NativeGpu_BeginFrontendFrame();
    s_frame_started = 1;
}
