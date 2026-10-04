/* Logging interface supplied by the Vita GPU adapter. */
#ifndef PCSX_VITA_GPU_NATIVE_LOG_H
#define PCSX_VITA_GPU_NATIVE_LOG_H

#include <macros.h>

void Platform_Log(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogWarn(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogError(const char *fmt, ...) CTR_PRINTF_FORMAT(1, 2);
void Platform_LogFlush(void);

#endif
