#!/bin/sh
# A picture of a level as the port draws it, without a window: after so
# many ticks of a recording.
#
#   examples/pushover/check/frame.sh finish/original/001_a 120 out.png
#
# Needs reference/build.sh and reference/pictures.sh to have run, and
# the compiler built (build/bin) with its LLVM (LLVM_PREFIX, as for
# tools/ent).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/pushover
game=$out/upstream
run=$out/run
mkdir -p "$run"
recording=$game/recordings/$1.rec
level=$(cd "$out" && ./ref/trace upstream/levels "$recording" 2> /dev/null | sed -n '1s/^level //p')
sed "s/Played.ticks = 100/Played.ticks = ${2:-100}/" "$here/frame_check.ent" > "$run/frame_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  build -I "$here/.." frame_check.ent -o frame_check)
cp "$out/$level" "$game/level.level"
cp "$recording" "$game/record.rec"
(cd "$game" && "$run/frame_check") > "$run/frame.out"
rm -f "$game/level.level" "$game/record.rec"
python3 -I "$here/frame.py" "$game" "${3:-$run/frame.png}" < "$run/frame.out"
