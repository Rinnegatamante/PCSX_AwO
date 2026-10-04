/* PS1 packet link conversion interface required by the GPU backend. */
#ifndef PCSX_VITA_GPU_NATIVE_GPU_LINKS_H
#define PCSX_VITA_GPU_NATIVE_GPU_LINKS_H

#include <stddef.h>
#include <stdint.h>

#define NATIVE_GPU_LINK_TERMINATOR 0x00ffffffu

uint32_t NativeGpuLinks_FromHostPointer(const void *hostPtr);
void *NativeGpuLinks_ToHostPointer(uint32_t token);
int NativeGpuLinks_IsTerminator(uint32_t token);
int NativeGpuLinks_IsRegisteredHostPointer(const void *hostPtr);
int NativeGpuLinks_IsRegisteredHostRange(const void *hostPtr, size_t size);
int NativeGpuLinks_RegisterRange(const void *hostStart, size_t size,
                                 uint32_t *tokenStartOut);
void NativeGpuLinks_RegisterRangeChecked(const char *label,
                                         const void *hostStart, size_t size);

#endif
