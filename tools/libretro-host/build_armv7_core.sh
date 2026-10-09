#!/bin/sh
# Build the System 12 libretro core for Linux ARMv7 (Cortex-A9 + NEON, the
# Vita's CPU flags and its ari64 ARM32 backend) and the headless host inside
# a linux/arm/v7 Docker container. On Apple silicon Docker runs it through
# qemu, so this checks ARM32 code paths without a Vita. Compare its
# --hash-ram/--capture-ppm/--wav output with build_mac_core.sh (ARM64):
# the dynarec backends must produce identical emulation.
#
#   tools/libretro-host/build_armv7_core.sh [OUT]   (default build/linux-armv7)
#   docker run --rm --platform linux/arm/v7 -v "$PWD:$PWD" -w "$PWD" \
#     pcsx-armv7-build OUT/s12-libretro-host --core OUT/src/pcsx_rearmed_libretro.so ...
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/linux-armv7}
case $OUT in /*) ;; *) OUT=$ROOT/$OUT ;; esac
IMAGE=pcsx-armv7-build
printf 'FROM debian:bookworm-slim\nRUN apt-get update && apt-get install -y --no-install-recommends build-essential && rm -rf /var/lib/apt/lists/*\n' |
  docker build --platform linux/arm/v7 -t "$IMAGE" -
mkdir -p "$OUT/src"
rsync -a --delete --exclude build --exclude build-vita --exclude artifacts \
  --exclude roms --exclude .git --exclude '*.o' --exclude '*.a' --exclude '*.vpk' \
  --exclude '*.elf' --exclude '*.velf' --exclude '*.dylib' --exclude '*.so' "$ROOT/" "$OUT/src/"
# NDRC_THREAD=0 keeps ari64 deterministic, as in build_mac_core.sh.
docker run --rm --platform linux/arm/v7 -v "$OUT:$OUT" -w "$OUT/src" \
  -e CFLAGS="-mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -marm -mfloat-abi=hard" "$IMAGE" sh -c '
  make -f Makefile.libretro platform=unix ARCADE_SYSTEM12=1 SYSTEM12_PROFILE=1 \
    DYNAREC=ari64 BUILTIN_GPU=neon GPU_NEON_USE_C=1 USE_ASYNC_GPU=0 USE_ASYNC_SPU=0 \
    USE_ASYNC_CDROM=0 HAVE_PHYSICAL_CDROM=0 NDRC_THREAD=0 -j8 &&
  gcc -O2 -g -std=gnu99 -Wall -Ideps/libretro-common/include \
    -o ../s12-libretro-host tools/libretro-host/s12_libretro_host.c -ldl'
echo "core: $OUT/src/pcsx_rearmed_libretro.so"
echo "host: $OUT/s12-libretro-host"
