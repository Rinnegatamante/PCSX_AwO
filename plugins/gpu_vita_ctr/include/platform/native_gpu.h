/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/gpu/PsyX_GPU.h
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#ifndef NATIVE_GPU_H
#define NATIVE_GPU_H

#include <macros.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>

typedef void (*NativeGpuBackendTaskFn)(void *arg);

extern DISPENV activeDispEnv;
extern DRAWENV activeDrawEnv;
extern int g_GPUDisabledState;

int NativeGpu_HasPendingSplits(void);
int NativeGpu_PendingSplitsTouch(int x, int y, int w, int h);
int NativeGpu_QueueUpload(const unsigned short *vram, int x, int y, int w, int h);
int NativeGpu_SubmitPartialFrame(void);
int NativeGpu_GuestVRAMStale(int x, int y, int w, int h);
void NativeGpu_GuestVRAMSynced(int x, int y, int w, int h);
void NativeGpu_GuestVRAMWritten(int x, int y, int w, int h);
void ClearSplits(void);
void DrawAllSplits(void);
void ParsePrimitivesLinkedList(u32 *p, int singlePrimitive);
int NativeGpu_GetStateSize(void);
int NativeGpu_CaptureState(void *dst, int dstSize);
int NativeGpu_RestoreState(const void *src, int srcSize);
int NativeGpu_InitBackend(int width, int height);
void NativeGpu_ShutdownBackend(void);
void NativeGpu_SyncBackend(void);
void NativeGpu_RunBackendTaskSync(NativeGpuBackendTaskFn task, void *arg);
void NativeGpu_BeginFrontendFrame(void);
void NativeGpu_SetFrontendDrawEnv(const DRAWENV *env);
void NativeGpu_SetFrontendDispEnv(const DISPENV *env);
void NativeGpu_SetFrontendDisplayBlank(int blank);
int NativeGpu_GetRenderDisplayBlank(void);
const DRAWENV *NativeGpu_GetRenderDrawEnv(void);
const DISPENV *NativeGpu_GetRenderDispEnv(void);
void NativeGpu_ForceSynchronousFrame(void);
void NativeGpu_FlushFrontendSplitsSync(void);
int NativeGpu_SubmitFrontendFrame(void);

#endif
