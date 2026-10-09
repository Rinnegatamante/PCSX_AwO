#!/bin/sh
# Callgrind (with a Cortex-A9-like cache model) of the ARMv7 replay of TRACE:
# per-symbol table in build/gpu-replay/prof/LABEL.txt. Needs the
# pcsx-armv7-valgrind image (pcsx-armv7-build + valgrind).
#   tools/gpu-replay/profile.sh TRACE LABEL [TOP]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=$1; L=$2; TOP=${3:-40}
P=build/gpu-replay/prof
mkdir -p $P
docker run --rm --platform linux/arm/v7 -v "$ROOT:$ROOT" -w "$ROOT" pcsx-armv7-valgrind sh -c "
  nm -n build/gpu-replay/gpu-replay-armv7 > $P/$L.nm
  valgrind --tool=callgrind --cache-sim=yes --D1=32768,4,32 --I1=32768,4,32 --LL=2097152,8,32 \
    --dump-instr=yes --dump-line=no --compress-pos=no --compress-strings=no \
    --callgrind-out-file=$P/$L.cg build/gpu-replay/gpu-replay-armv7 $T --no-frame-hash 2>&1 | grep gpu-replay:"
python3 tools/gpu-replay/cg_by_symbol.py $P/$L.cg $P/$L.nm $TOP > $P/$L.txt
head -3 $P/$L.txt
