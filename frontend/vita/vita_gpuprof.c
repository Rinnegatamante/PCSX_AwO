/*
 * GPU profiler captures (psp2-cex-gpues4-prof) for the vitaGL renderer, ported
 * from geometrizer's platform/vita/vita_gpuprof.c; see vita_gpuprof.h.
 *
 * Threads: the observer runs on whichever thread calls GXM (the "CTR Renderer"
 * thread: vitaGL's draws and the present in vglSwapBuffers), the hardware
 * sampler on its own thread; the capture's mutex orders the records. State
 * changes happen on the renderer thread only (the observer during the present,
 * vita_gpuprof_after_frame after it).
 */

#include "vita_gpuprof.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <psp2_gpuprof.h>
#include <psp2_gpuprof_capture_diagnostic.h>
#include <psp2_gpuprof_diagnostic.h>
#include <psp2_gpuprof_gxm_observer.h>
#include <psp2_gpuprof_labels.h>
#include <psp2_gpuprof_vitagl.h>

#include <psp2/kernel/clib.h>

#include <stdio.h>
#include <stdlib.h>

#define CSV_PATH "ux0:data/pcsx-rearmed/gpuprof.csv"
#define TRIGGER_PATH "ux0:data/pcsx-rearmed/gpuprof"
#define LOG_ERROR(tag, fmt, ...) sceClibPrintf("pcsx-vita: " fmt "\n", ##__VA_ARGS__)

/* The trigger file's text (n >= 0) or -1 when it does not exist. */
static int trigger_read(char *buf, int size)
{
    FILE *f = fopen(TRIGGER_PATH, "r");
    if (!f)
        return -1;
    int n = (int)fread(buf, 1, (size_t)size - 1, f);
    fclose(f);
    buf[n > 0 ? n : 0] = 0;
    return n > 0 ? n : 0;
}

enum { IDLE, ARMED, RECORDING, STOPPING };
enum { HISTORY = 4096 };

static struct {
    /* GXM's identity counters, kept from its first call on. */
    uint32_t frames, scenes;
    int state;
    int stop_requested;
    unsigned from, to;
    const char *id;
    Psp2GpuProfCaptureEvent *events;
    size_t capacity;
    Psp2GpuProfCapture capture;
    Psp2GpuProfLabels labels;
    int labels_on;
    int scope_ok[PSP2_GPUPROF_SCOPE_DEPTH];
    unsigned depth;
    SceUID mutex;
    SceUID sampler;
    volatile int sampler_stop;
    uint32_t fingerprint;
    unsigned samples, read_failures;
    /* The object lifetimes before the capture (GXM contexts and shader
     * programs), replayed into it right after the seed: the reader needs the
     * creation of every context and program the window binds. */
    Psp2GpuProfGxmCall history[HISTORY];
    unsigned history_n;
    int history_lost;
    int history_off;   /* no more history: after the capture (one per launch), or given up */
    SceUID history_mutex;
} s = { .mutex = -1, .sampler = -1, .history_mutex = -1 };

static void lock(void *p) { (void)p; sceKernelLockMutex(s.mutex, 1, NULL); }
static void unlock(void *p) { (void)p; sceKernelUnlockMutex(s.mutex, 1); }

static int lifetime_call(unsigned kind)
{
    return (kind >= PSP2_GPUPROF_GXM_CONTEXT_CREATE && kind <= PSP2_GPUPROF_GXM_PATCHER_DESTROY) ||
           kind == PSP2_GPUPROF_GXM_MASK_FRAGMENT_CREATE;
}

/* The program a lifetime call is about, as the reader keys it: (stage,
 * patcher, handle); 0 for context and patcher calls. */
static int program_key(const Psp2GpuProfGxmCall *c, uintptr_t key[3])
{
    const unsigned k = c->kind;
    if (k < PSP2_GPUPROF_GXM_VERTEX_CREATE || (k > PSP2_GPUPROF_GXM_FRAGMENT_RELEASE &&
                                               k != PSP2_GPUPROF_GXM_MASK_FRAGMENT_CREATE))
        return 0;
    const int create = k == PSP2_GPUPROF_GXM_VERTEX_CREATE || k == PSP2_GPUPROF_GXM_FRAGMENT_CREATE ||
                       k == PSP2_GPUPROF_GXM_MASK_FRAGMENT_CREATE;
    key[0] = (k == PSP2_GPUPROF_GXM_VERTEX_CREATE || k == PSP2_GPUPROF_GXM_VERTEX_ADDREF ||
              k == PSP2_GPUPROF_GXM_VERTEX_RELEASE) ? 1 : 2;
    key[1] = c->args[0];
    key[2] = create ? c->args[2] : c->args[1];
    return 1;
}

/* History full: drop the programs whose references all went (their create,
 * addrefs and releases) and the patchers destroyed with everything they made. */
static void history_compact(void)
{
    unsigned n = s.history_n, out = 0;
    static unsigned char dead[HISTORY];
    for (unsigned i = 0; i < n; i++)
        dead[i] = 0;
    for (unsigned i = 0; i < n; i++) {
        const Psp2GpuProfGxmCall *c = &s.history[i];
        uintptr_t key[3], other[3];
        if (c->kind == PSP2_GPUPROF_GXM_PATCHER_DESTROY) {
            dead[i] = 1;
            for (unsigned j = 0; j < i; j++)
                if (program_key(&s.history[j], other) && other[1] == c->args[0])
                    dead[j] = 1;
            continue;
        }
        const unsigned k = c->kind;
        if (!program_key(c, key) || dead[i] ||
            !(k == PSP2_GPUPROF_GXM_VERTEX_CREATE || k == PSP2_GPUPROF_GXM_FRAGMENT_CREATE ||
              k == PSP2_GPUPROF_GXM_MASK_FRAGMENT_CREATE))
            continue;
        /* Walk this program's life; if its references reach zero, it is dead. */
        int refs = 1;
        unsigned end = i;
        for (unsigned j = i + 1; j < n && refs > 0; j++) {
            if (!program_key(&s.history[j], other) || other[0] != key[0] ||
                other[1] != key[1] || other[2] != key[2])
                continue;
            const unsigned kj = s.history[j].kind;
            if (kj == PSP2_GPUPROF_GXM_VERTEX_RELEASE || kj == PSP2_GPUPROF_GXM_FRAGMENT_RELEASE)
                refs--;
            else if (kj == PSP2_GPUPROF_GXM_VERTEX_ADDREF || kj == PSP2_GPUPROF_GXM_FRAGMENT_ADDREF)
                refs++;
            else
                break;   /* created again while live: leave it to the reader */
            end = j;
        }
        if (refs == 0)
            for (unsigned j = i; j <= end; j++)
                if (program_key(&s.history[j], other) && other[0] == key[0] &&
                    other[1] == key[1] && other[2] == key[2])
                    dead[j] = 1;
    }
    for (unsigned i = 0; i < n; i++)
        if (!dead[i])
            s.history[out++] = s.history[i];
    s.history_n = out;
}

static void history_add(const Psp2GpuProfGxmCall *c)
{
    sceKernelLockMutex(s.history_mutex, 1, NULL);
    if (s.state == RECORDING) {   /* the seed came in between (another thread) */
        psp2GpuProfCaptureGxmCall(&s.capture, c);
        sceKernelUnlockMutex(s.history_mutex, 1);
        return;
    }
    if (s.history_off) {
        sceKernelUnlockMutex(s.history_mutex, 1);
        return;
    }
    if (s.history_n == HISTORY) {
        /* The compaction is quadratic: run it only when it pays. vitaGL creates and
         * releases fragment programs all the time, so a history that stays nearly
         * full after compacting would compact on every call (a long session went
         * from 2 to 16 ms of draw); give it up instead: a later capture is then
         * reported with incomplete lifetimes. */
        history_compact();
        if (s.history_n > HISTORY * 3 / 4) {
            s.history_lost = 1;
            s.history_off = 1;
        }
    }
    if (!s.history_off)
        s.history[s.history_n++] = *c;
    sceKernelUnlockMutex(s.history_mutex, 1);
}

/* Every GXM call: record it while recording, keep the lifetime calls before;
 * count presents and scenes always; seed at the present after arming, stop at
 * the present after the stop. */
static void observe(void *user, const Psp2GpuProfGxmCall *c)
{
    (void)user;
    if (s.state == RECORDING)
        psp2GpuProfCaptureGxmCall(&s.capture, c);
    else if (!c->result && lifetime_call(c->kind))
        history_add(c);
    if (c->result)
        return;
    if (c->kind == PSP2_GPUPROF_GXM_END) {
        s.scenes++;
    } else if (c->kind == PSP2_GPUPROF_GXM_PRESENT) {
        s.frames = (s.frames + 1) & 0xffffffu;
        s.scenes = 0;
        if (s.state == ARMED) {
            psp2GpuProfCaptureGxmIdentitySeed365(&s.capture, (uint32_t)sceKernelGetProcessId(),
                                                 s.frames, 0);
            sceKernelLockMutex(s.history_mutex, 1, NULL);
            for (unsigned i = 0; i < s.history_n; i++)
                psp2GpuProfCaptureGxmCall(&s.capture, &s.history[i]);
            s.state = RECORDING;
            sceKernelUnlockMutex(s.history_mutex, 1);
        } else if (s.state == RECORDING &&
                   (s.stop_requested || s.capture.count + 8192 >= s.capacity)) {
            s.state = STOPPING;
        }
    }
}

void vita_gpuprof_install(void)
{
    s.history_mutex = sceKernelCreateMutex("pcsx_gpuprof_history", 0, 0, NULL);
    psp2GpuProfGxmSetObserver(observe, NULL);
}

static int sample_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static const unsigned groups[] = { 2, 4, 43, 55, 71, 72, 75, 76, 77, 78, 103, 104, 107, 108, 109, 110 };
    for (uint64_t id = 0; !s.sampler_stop; id++) {
        const unsigned group = groups[id % 16];
        Psp2GpuProfDiagnosticConfig cfg = { .size = sizeof cfg, .abi = PSP2_GPUPROF_ABI,
                                            .group = group, .tag_group = group >= 103 ? 102 : 70 };
        Psp2GpuProfDiagnostic data = {0};
        Psp2GpuProfDiagnosticObservation o = {
            .sampler_thread = (uint32_t)sceKernelGetThreadId(), .sample_id = id,
            .group = group, .tag_group = cfg.tag_group,
            .process_id = (uint32_t)sceKernelGetProcessId(), .driver_fingerprint = s.fingerprint };
        int clock = scePowerGetGpuClockFrequency();
        o.clock_before_mhz = clock > 0 ? (unsigned)clock : 0;
        o.before_us = sceKernelGetSystemTimeWide();
        o.result = psp2GpuProfReadDiagnostic(&cfg, &data);
        o.after_us = sceKernelGetSystemTimeWide();
        clock = scePowerGetGpuClockFrequency();
        o.clock_after_mhz = clock > 0 ? (unsigned)clock : 0;
        if (o.result)
            s.read_failures++;
        if (!psp2GpuProfCaptureDiagnostic(&s.capture, &o, o.result ? NULL : &data))
            s.samples++;
        sceKernelDelayThread(500);
    }
    return 0;
}

static void sampler_start(void)
{
    Psp2GpuProfInfo info = { .size = sizeof info, .abi = PSP2_GPUPROF_ABI };
    int rc = psp2GpuProfGetInfo(&info);
    if (rc || info.status || !(info.capabilities & PSP2_GPUPROF_CAP_DIAGNOSTIC)) {
        LOG_ERROR(TAG, "gpuprof: plugin not ready (rc=0x%08x status=0x%08x caps=0x%x): no hardware samples",
                  (unsigned)rc, (unsigned)info.status, (unsigned)info.capabilities);
        return;
    }
    s.fingerprint = info.driver_fingerprint;
    s.sampler_stop = 0;
    /* Core 2 (the board thread's, mostly waiting): the emulation is on core 0. */
    s.sampler = sceKernelCreateThread("pcsx_gpuprof", sample_thread, 0x10000100, 0x10000,
                                      0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s.sampler >= 0 && sceKernelStartThread(s.sampler, 0, NULL) < 0) {
        sceKernelDeleteThread(s.sampler);
        s.sampler = -1;
    }
}

static void sampler_stop(void)
{
    if (s.sampler < 0)
        return;
    s.sampler_stop = 1;
    sceKernelWaitThreadEnd(s.sampler, NULL, NULL);
    sceKernelDeleteThread(s.sampler);
    s.sampler = -1;
}

void vita_gpuprof_session_begin(const char *id)
{
    char buf[48];
    const int n = trigger_read(buf, sizeof buf);
    s.state = IDLE;
    s.from = s.to = 0;
    if (n < 0)
        return;
    unsigned from = 600, to = 660, events = 65536;
    if (n > 0)
        sscanf(buf, "%u,%u,%u", &from, &to, &events);
    if (to <= from || events < 16384) {
        LOG_ERROR(TAG, "gpuprof: bad trigger \"%s\" (start,stop[,events])", buf);
        return;
    }
    if (s.mutex < 0)
        s.mutex = sceKernelCreateMutex("pcsx_gpuprof", 0, 0, NULL);
    if (!s.events) {
        /* Kept for the life of the process: vitaGL's workers may still call
         * GXM after a capture, and a stopped capture rejects their records. */
        for (; events >= 16384; events /= 2)
            if ((s.events = malloc(events * sizeof *s.events)) != NULL)
                break;
        if (!s.events) {
            LOG_ERROR(TAG, "gpuprof: no memory for the event buffer");
            return;
        }
        s.capacity = events;
    }
    Psp2GpuProfCaptureConfig cfg = { s.events, s.capacity, lock, unlock, NULL };
    if (psp2GpuProfCaptureInit(&s.capture, &cfg)) {
        LOG_ERROR(TAG, "gpuprof: capture init failed");
        return;
    }
    s.from = from;
    s.to = to;
    s.id = id;
    LOG_ERROR(TAG, "gpuprof: %s frames %u..%u, %u events (%u KB)", id, from, to,
              (unsigned)s.capacity, (unsigned)(s.capacity * sizeof *s.events / 1024));
}

static void finish(void)
{
    sceKernelLockMutex(s.history_mutex, 1, NULL);
    s.history_off = 1;   /* one capture per launch: nothing will need the history again */
    sceKernelUnlockMutex(s.history_mutex, 1);
    s.labels_on = 0;
    psp2GpuProfVitaGLCapture(NULL);
    sampler_stop();
    s.state = IDLE;
    psp2GpuProfCaptureStop(&s.capture);
    FILE *f = fopen(CSV_PATH, "w");
    static char iobuf[256 * 1024];   /* ux0 is slow with small writes */
    if (f)
        setvbuf(f, iobuf, _IOFBF, sizeof iobuf);
    int rc = f ? psp2GpuProfCaptureWriteCsv(&s.capture, f) : PSP2_GPUPROF_CAPTURE_IO;
    if (f && fclose(f))
        rc = PSP2_GPUPROF_CAPTURE_IO;
    LOG_ERROR(TAG, "gpuprof: %s -> " CSV_PATH " rc=%d events=%u dropped=%u samples=%u read_failures=%u history=%u%s",
              s.id, rc, (unsigned)s.capture.count, (unsigned)s.capture.dropped, s.samples,
              s.read_failures, s.history_n, s.history_lost ? " (LOST: lifetimes incomplete)" : "");
    s.from = s.to = 0;
}

void vita_gpuprof_after_frame(unsigned frame)
{
    if (!s.to)
        return;
    if (s.state == IDLE && frame == s.from) {
        s.stop_requested = 0;
        s.samples = s.read_failures = 0;
        if (psp2GpuProfCaptureStart(&s.capture) == 0)
            s.state = ARMED;   /* the next present seeds it */
        return;
    }
    if (s.state == RECORDING) {
        if (!s.labels_on) {   /* the first frame after the seed */
            psp2GpuProfLabelsBegin(&s.labels, &s.capture, (uint64_t)sceKernelGetThreadId());
            s.depth = 0;
            s.labels_on = 1;
            psp2GpuProfVitaGLCapture(&s.capture);
            sampler_start();
        }
        if (frame >= s.to)
            s.stop_requested = 1;   /* the next present stops it */
        psp2GpuProfFrameName(&s.labels, sceKernelGetSystemTimeWide(), frame + 1, s.id);
        return;
    }
    if (s.state == STOPPING)
        finish();
}

void vita_gpuprof_session_end(void)
{
    if (s.state == RECORDING || s.state == STOPPING)
        finish();
    else if (s.state == ARMED)
        psp2GpuProfCaptureStop(&s.capture);
    s.state = IDLE;
    s.from = s.to = 0;
}
