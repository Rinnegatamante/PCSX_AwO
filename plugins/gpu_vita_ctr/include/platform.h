/* Platform functions implemented by the Vita GPU adapter in gpulib_if.c. */
#ifndef PCSX_VITA_GPU_PLATFORM_H
#define PCSX_VITA_GPU_PLATFORM_H

int Platform_BeginScene(void);
void Platform_EndScene(void);
void Platform_PollHostEvents(void);

#endif
