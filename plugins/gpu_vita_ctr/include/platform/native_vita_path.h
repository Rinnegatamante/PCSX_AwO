/*
 * Select the Vita rendering and batching semantics independently from the
 * Vita SDK and runtime ABI. Native Vita builds imply this path; host harnesses
 * may opt in explicitly with -DPCSX_NATIVE_VITA_PATH=1.
 */
#ifndef PCSX_NATIVE_VITA_PATH_H
#define PCSX_NATIVE_VITA_PATH_H

#if defined(__vita__) && !defined(PCSX_NATIVE_VITA_PATH)
#define PCSX_NATIVE_VITA_PATH 1
#endif

#endif
