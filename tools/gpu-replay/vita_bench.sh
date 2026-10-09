#!/bin/sh
# Time the GPU replay on a PS Vita reached through vitacompanion (FTP 1337,
# commands 1338): installs build/gpu-replay/vita/eboot.bin over TITLE_ID's
# eboot (put the app's own eboot back afterwards), uploads TRACE if needed,
# runs it with ARGS and prints the result; then launches Geometrizer.
#   VITA=192.168.0.25 TITLE_ID=PCSXN0012 tools/gpu-replay/vita_bench.sh TRACE [ARGS...]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
H=${VITA:-192.168.0.25}; V=ftp://$H:1337; T=${TITLE_ID:-PCSXN0012}
D=ux0:data/pcsx-rearmed
TRACE=$1; shift
EBOOT=$ROOT/build/gpu-replay/vita/eboot.bin
name=$(basename "$TRACE")
printf 'nosleep on\n' | nc -w 3 $H 1338 >/dev/null
printf 'destroy\n' | nc -w 3 $H 1338 >/dev/null; sleep 3
curl -s -T "$EBOOT" "$V/ux0:/app/$T/eboot.bin"
[ "$(curl -s "$V/ux0:/app/$T/" | awk '/eboot.bin/{print $5}')" = "$(stat -f %z "$EBOOT")" ] || { echo "eboot upload failed"; exit 1; }
if [ "$(curl -s "$V/$D/" | awk -v n="$name" '$NF==n{print $5}')" != "$(stat -f %z "$TRACE")" ]; then
  curl -s -T "$TRACE" "$V/$D/$name"
fi
printf '%s\n' "$D/$name $*" | curl -s -T - "$V/$D/replay-args.txt"
curl -s -Q "DELE $D/replay-out.txt" "$V/" >/dev/null 2>&1 || true
printf 'launch %s\n' "$T" | nc -w 3 $H 1338 >/dev/null
i=0
while [ $i -lt 120 ]; do
  sleep 3; i=$((i + 1))
  out=$(curl -s --max-time 5 "$V/$D/replay-out.txt" || true)
  case $out in *MATCH*) break ;; esac
done
echo "$out"
printf 'destroy\n' | nc -w 3 $H 1338 >/dev/null; sleep 2
printf 'launch GEOM00001\n' | nc -w 3 $H 1338 >/dev/null
