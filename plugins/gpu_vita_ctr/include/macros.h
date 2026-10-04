/*
 * Narrow Vita GPU compatibility definitions for the vendored PsyCross headers.
 * The original CTR Native macros.h belongs to a GPL-3.0-only project; this
 * file intentionally supplies only the plain types and compiler annotations
 * required by the MIT-derived GPU interface headers and this backend.
 */
#ifndef PCSX_VITA_GPU_MACROS_H
#define PCSX_VITA_GPU_MACROS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef float f32;
typedef double f64;
typedef s8 b8;
typedef s16 b16;
typedef s32 b32;

#if defined(__GNUC__) || defined(__clang__)
#define CTR_MAY_ALIAS __attribute__((may_alias))
#define CTR_PRINTF_FORMAT(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define CTR_MAY_ALIAS
#define CTR_PRINTF_FORMAT(format_index, first_arg)
#endif

#define CTR_JOIN_INNER(a, b) a##b
#define CTR_JOIN(a, b) CTR_JOIN_INNER(a, b)
#define CTR_STATIC_ASSERT(expression) \
    typedef char CTR_JOIN(pcsx_vita_gpu_static_assert_, __LINE__)[(expression) ? 1 : -1]

#define global_variable static
#define local_persist static
#define internal static
#define len(array) (sizeof(array) / sizeof((array)[0]))
#define OFFSETOF(type, member) ((u32)offsetof(type, member))
#define CTR_OFFSET_OF_ARRAY(type, member, index) \
    (offsetof(type, member) + (u32)(index) * sizeof(((type *)0)->member[0]))
#define CTR_OFFSET_OF_2D_ARRAY(type, member, row, column) \
    (offsetof(type, member) + (u32)(row) * sizeof(((type *)0)->member[0]) + \
     (u32)(column) * sizeof(((type *)0)->member[0][0]))

#ifndef nullptr
#define nullptr ((void *)0)
#endif

static inline u32 CTR_ReadU32LE(const void *src)
{
    const u8 *p = (const u8 *)src;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline void CTR_WriteU32LE(void *dst, u32 value)
{
    u8 *p = (u8 *)dst;
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static inline void CTR_WriteU32AlignedLE(void *dst, u32 value)
{
    *(u32 *)dst = value;
}

#endif
