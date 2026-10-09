#!/bin/zsh
# PC-sampling profile of the GPU replay on the Vita with vita-tracy (skill
# vita-tracy-profiling in ~/Fun/model): build with `TRACY=1 build.sh vita`
# first. Loads the kernel plugin (needed once per boot), installs the eboot
# over TITLE_ID's, replays TRACE with ARGS while capturing SECS seconds, and
# writes OUT.tracy, OUT.txt (samples) and OUT.elf for tprof.py. Then launches
# Geometrizer.
#   tools/gpu-replay/vita_tracy.sh TRACE SECS OUT [ARGS...]
set -eu
ROOT=${0:A:h:h:h}
TRACE=$1 SECS=$2 OUT=$3; shift 3
H=${VITA:-192.168.0.25}; FTP=ftp://$H:1337; T=${TITLE_ID:-PCSXN0012}; D=ux0:data/pcsx-rearmed
TT=$HOME/Fun/model/tools/vita_tracy
CAP=${VITA_TRACE:-$HOME/Fun/vitasdk-next/vita-trace}/third_party/tracy/capture/build/tracy-capture
cmd() { printf '%s\n' "$1" | nc -w 5 $H 1338 >/dev/null; }
cmd 'nosleep on'
[ "${SKIP_PLUGIN:-0}" = 1 ] || VITA=$H $TT/capture.sh plugin | grep -i "load\|0x" | head -2
EB=$ROOT/build/gpu-replay/vita/eboot.bin
cmd destroy; sleep 3
curl -s -T $EB "$FTP/ux0:/app/$T/eboot.bin"
[[ $(curl -s "$FTP/ux0:/app/$T/" | awk '/eboot.bin/{print $5}') == $(stat -f %z $EB) ]] || { echo "eboot upload failed"; exit 1; }
name=${TRACE:t}
[[ $(curl -s "$FTP/$D/" | awk -v n=$name '$NF==n{print $5}') == $(stat -f %z $TRACE) ]] || curl -s -T $TRACE "$FTP/$D/$name"
printf '%s\n' "$D/$name $*" | curl -s -T - "$FTP/$D/replay-args.txt"
cp $ROOT/build/gpu-replay/vita/gpu-replay.elf ${OUT}.elf
cmd 'screen on'; cmd "launch $T"
sleep 3
$CAP -a $H -o ${OUT}.tracy -s $SECS | tail -2
$TT/tsamples ${OUT}.tracy > ${OUT}.txt
echo "samples: $(grep -c '^S' ${OUT}.txt)"
curl -s --max-time 5 "$FTP/$D/replay-out.txt"
cmd destroy; sleep 2; cmd 'launch GEOM00001'
