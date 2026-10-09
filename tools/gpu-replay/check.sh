#!/bin/sh
# Replay every build/gpu-replay/*.gtr with the macOS and ARMv7 builds and
# compare flip/VRAM hashes with BASELINE (default build/gpu-replay/baseline.txt;
# --update rewrites it). Exit status 1 on any MISMATCH or baseline difference.
#   tools/gpu-replay/check.sh [--update] [--no-build]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT=build/gpu-replay
BASE=${BASELINE:-$OUT/baseline.txt}
UPDATE=0; BUILD=1
for a in "$@"; do
  case $a in --update) UPDATE=1 ;; --no-build) BUILD=0 ;; esac
done
if [ $BUILD = 1 ]; then
  tools/gpu-replay/build.sh mac >/dev/null
  tools/gpu-replay/build.sh armv7 >/dev/null
fi
RES=$OUT/check.txt
: > $RES
for t in $OUT/*.gtr; do
  n=$(basename $t .gtr)
  m=$($OUT/gpu-replay-mac $t | sed 's/ [0-9.]* ms\/loop//')
  a=$(docker run --rm --platform linux/arm/v7 -v "$ROOT:$ROOT" -w "$ROOT" pcsx-armv7-build \
      $OUT/gpu-replay-armv7 $t | sed 's/ [0-9.]* ms\/loop//')
  echo "$n mac   $m" >> $RES
  echo "$n armv7 $a" >> $RES
  # gpu_neon's two-stage renderer must give the same frames and VRAM
  mp=$($OUT/gpu-replay-mac $t --pipeline 2>/dev/null | sed 's/ [0-9.]* ms\/loop//')
  ap=$(docker run --rm --platform linux/arm/v7 -v "$ROOT:$ROOT" -w "$ROOT" pcsx-armv7-build \
      $OUT/gpu-replay-armv7 $t --pipeline 2>/dev/null | sed 's/ [0-9.]* ms\/loop//')
  [ "$mp" = "$m" ] || echo "$n mac   pipeline differs: $mp" >> $RES
  [ "$ap" = "$a" ] || echo "$n armv7 pipeline differs: $ap" >> $RES
done
cat $RES
if [ $UPDATE = 1 ]; then cp $RES $BASE; echo "baseline updated"; exit 0; fi
st=0
grep -q "MISMATCH\|differs" $RES && { echo "FAIL: replay mismatch"; st=1; }
if [ -f $BASE ]; then
  diff $BASE $RES >/dev/null && echo "OK: identical to baseline" || { echo "FAIL: differs from baseline"; diff $BASE $RES; st=1; }
fi
exit $st
