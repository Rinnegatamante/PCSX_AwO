#!/bin/sh
# Build c352-replay: build.sh mac -> build/c352-replay/c352-replay-mac (ARM64),
# build.sh armv7 -> build/c352-replay/c352-replay-armv7 (Cortex-A9 + NEON, in
# the pcsx-armv7-build container). Extra compiler flags come from $CFLAGS.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/build/c352-replay
mkdir -p "$OUT"
cd "$ROOT"
SRC="tools/c352-replay/c352_replay.c libpcsxcore/arcade/c352.c"
case ${1:-mac} in
mac)
  xcrun clang -arch arm64 -O2 -g -std=gnu99 -Wall ${CFLAGS:-} -o "$OUT/c352-replay-mac" $SRC
  echo "$OUT/c352-replay-mac" ;;
armv7)
  docker run --rm --platform linux/arm/v7 -v "$ROOT:$ROOT" -w "$ROOT" pcsx-armv7-build \
    gcc -O2 -g -std=gnu99 -Wall -mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -marm -mfloat-abi=hard \
    ${CFLAGS:-} -o "$OUT/c352-replay-armv7" $SRC
  echo "$OUT/c352-replay-armv7" ;;
*) echo "usage: $0 mac|armv7" >&2; exit 1 ;;
esac
