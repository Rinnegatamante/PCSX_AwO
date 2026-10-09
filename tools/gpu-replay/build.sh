#!/bin/sh
# Build gpu-replay with the System 12 NEON renderer:
#   tools/gpu-replay/build.sh mac    -> build/gpu-replay/gpu-replay-mac   (ARM64, C/SIMD renderer)
#   tools/gpu-replay/build.sh armv7  -> build/gpu-replay/gpu-replay-armv7 (Cortex-A9 NEON assembly,
#                                       in the pcsx-armv7-build container; see build_armv7_core.sh)
#   tools/gpu-replay/build.sh vita   -> build/gpu-replay/vita/eboot.bin (PS Vita, VitaSDK)
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
vita)
  # Replaces an installed app's eboot (TITLE_ID, default PCSXN0012) for
  # timing on the console: see tools/gpu-replay/vita_bench.sh.
  V=$OUT/vita; mkdir -p "$V"
  PATH=${VITASDK:-/usr/local/vitasdk}/bin:$PATH
  # TRACY=1 links vita-trace's client (PC sampling: tools/gpu-replay/vita_tracy.sh),
  # taken from geometrizer's tracy build as the emulator's tracy builds do.
  EXTRA=""; LINK=arm-vita-eabi-gcc
  if [ "${TRACY:-0}" = 1 ]; then
    B=${GEO_TRACY_BUILD:-$HOME/Fun/model/build-vita-tracy}
    O=$B/CMakeFiles/geometrizer.dir/Users/frangar/Fun/vitasdk-next/vita-trace/client
    EXTRA="$O/tracy_vita_autostart.c.obj $O/tracy_vita_autoemit.cpp.obj $B/vita-trace/client/libtracy_vita.a \
      $B/vita-trace/kernel/tracy_kernel_stubs/libVitaTracyKernel_stub_weak.a -lScePerf_stub_weak \
      -lSceKernelDmacMgr_stub -lSceNet_stub -lSceNetCtl_stub -lSceSysmodule_stub \
      -Wl,--wrap=main -Wl,--wrap=sceDisplaySetFrameBuf -Wl,--wrap=sceKernelExitProcess"
    LINK=arm-vita-eabi-g++
  fi
  rm -rf "$V/obj"; mkdir -p "$V/obj"
  for f in $SRC plugins/gpu_neon/psx_gpu/psx_gpu_arm_neon.S; do
    arm-vita-eabi-gcc -c -O3 -g -std=gnu99 -w -DVITA -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -marm \
      -fsingle-precision-constant -ffast-math $DEFS ${CFLAGS:-} -Iinclude -Ideps/libretro-common/include \
      -o "$V/obj/$(basename "$f").o" "$f"
  done
  $LINK -Wl,-q -o "$V/gpu-replay.elf" "$V"/obj/*.o $EXTRA \
    -lpthread -lSceLibKernel_stub -lSceKernelThreadMgr_stub -lScePower_stub -lSceDisplay_stub -lc
  vita-elf-create "$V/gpu-replay.elf" "$V/gpu-replay.velf" >/dev/null
  vita-make-fself -c -s -na "$V/gpu-replay.velf" "$V/eboot.bin" >/dev/null
  echo "$V/eboot.bin" ;;
*) echo "usage: $0 mac|armv7|vita" >&2; exit 1 ;;
esac
