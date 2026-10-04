/* Small standard-library compatibility layer for the upstream MAME core. */
#ifndef PCSX_ARCADE_TMS57002_COMPAT_H
#define PCSX_ARCADE_TMS57002_COMPAT_H
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#define popmessage(...) ((void)0)
#define logerror(...) ((void)0)

using u8 = uint8_t;
using s8 = int8_t;
using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using s64 = int64_t;
#endif
