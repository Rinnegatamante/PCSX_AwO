#!/bin/sh
# Build gpu-replay with the System 12 NEON renderer:
#   tools/gpu-replay/build.sh mac    -> build/gpu-replay/gpu-replay-mac   (ARM64, C/SIMD renderer)
#   tools/gpu-replay/build.sh armv7  -> build/gpu-replay/gpu-replay-armv7 (Cortex-A9 NEON assembly,
#                                       in the pcsx-armv7-build container; see build_armv7_core.sh)
# Extra compiler flags come from $CFLAGS.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/build/gpu-replay
mkdir -p "$OUT"
DEFS="-DPCSX_SYSTEM12 -DARCADE_SYSTEM12 -DGPU_NEON -DNEON_BUILD -DTEXTURE_CACHE_4BPP -DTEXTURE_CACHE_8BPP -DPSX_GPU_PIPELINE"
SRC="tools/gpu-replay/gpu_replay.c plugins/gpulib/gpu.c plugins/gpulib/vout_pl.c plugins/gpulib/prim.c plugins/gpu_neon/psx_gpu_if.c frontend/pcsxr-threads.c deps/libretro-common/features/features_cpu.c deps/libretro-common/compat/compat_strl.c"
cd "$ROOT"
case ${1:-mac} in
mac)
  xcrun clang -arch arm64 -O2 -g -std=gnu99 -w $DEFS -DSIMD_BUILD ${CFLAGS:-} -Iinclude \
    -Ideps/libretro-common/include -lpthread -o "$OUT/gpu-replay-mac" $SRC plugins/gpu_neon/psx_gpu/psx_gpu_simd.c
  echo "$OUT/gpu-replay-mac" ;;
armv7)
  docker run --rm --platform linux/arm/v7 -v "$ROOT:$ROOT" -w "$ROOT" pcsx-armv7-build \
    gcc -O2 -g -std=gnu99 -w -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -marm -mfloat-abi=hard \
    $DEFS ${CFLAGS:-} -Iinclude -Ideps/libretro-common/include -o "$OUT/gpu-replay-armv7" $SRC \
    plugins/gpu_neon/psx_gpu/psx_gpu_arm_neon.S -lpthread
  echo "$OUT/gpu-replay-armv7" ;;
*) echo "usage: $0 mac|armv7" >&2; exit 1 ;;
esac
