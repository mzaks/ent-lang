#!/bin/sh
# The screen as the port draws it, against the reference's, pixel for
# pixel, after some tics of a demo of the WAD (after reference/fetch.sh):
#   check/shots.sh 3 0,50,100,300,600      demo3, after those tics
# The screens are in build/doom/run as shots.reference.raw and
# shots.port; check/png.py makes pictures of them to look at. Needs the
# compiler built (build/bin) and its LLVM (LLVM_PREFIX, as for
# tools/ent).
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run
number=${1:-3}
tics=${2:-0,50,100,300,600}
cp "$doom/wad/doom1.wad" "$run/doom1.wad"
# (The whole view above the status bar, and no messages.)
printf 'screenblocks 10\nshow_messages 0\ndetaillevel 0\n' > "$run/shots.cfg"
(cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  DOOM_SHOTS="$run/shots.reference.raw" DOOM_SHOT_TICS=$tics \
  "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
    -config "$run/shots.cfg" -timedemo "demo$number" -nosound 2> /dev/null |
  grep '^shot ' > "$run/shots.tics")
{
  echo "table shots: i32 = ["
  awk '{ printf "%s%s", (NR > 1 ? ", " : "  "), $2 }' "$run/shots.tics"
  printf '\n]\n'
} > "$run/shots.ent"
rm -f "$run/shots.port"
sed "s/DEMO1/DEMO$number/" "$here/shots_check.ent" > "$run/shots_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  run -I "$here/.." -I "$run" "$run/shots_check.ent")
echo "after the tics$(awk '{ printf " %s", $2 }' "$run/shots.tics"):"
python3 "$here/pictures.py" "$run/shots.reference.raw" "$run/shots.port" ${3:-0}
