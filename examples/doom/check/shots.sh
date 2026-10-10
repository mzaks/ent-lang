#!/bin/sh
# The screen as the port draws it, against the reference's, pixel for
# pixel, after some tics of a demo of the WAD (after reference/fetch.sh):
#   check/shots.sh 3 0,50,100,300,600      demo3, after those tics
#   check/shots.sh e1m5_uv_max 1600,3700   a demo of reference/fetch_demos.sh
# Which of the palettes each screen is shown in is compared as well.
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
demo=demo$number
lump=DEMO$number
if [ -f "$doom/demos/$number.lmp" ]; then
  # (A demo of its own: for the reference as a file, for the port in the
  # place of DEMO1 in a copy of the WAD.)
  demo=$number
  lump=DEMO1
  cp "$doom/demos/$number.lmp" "$run/$number.lmp"
  python3 -I - "$doom/wad/doom1.wad" "$run/$number.lmp" "$run/doom1.wad" <<'END'
import struct, sys
wad = bytearray(open(sys.argv[1], "rb").read())
demo = open(sys.argv[2], "rb").read()
count, table = struct.unpack_from("<ii", wad, 4)
for lump in range(count):
    entry = table + lump * 16
    if wad[entry + 8:entry + 16].rstrip(b"\0") == b"DEMO1":
        struct.pack_into("<ii", wad, entry, len(wad), len(demo))
        break
open(sys.argv[3], "wb").write(wad + demo)
END
fi
# (The whole view above the status bar, and no messages.)
printf 'screenblocks 10\nshow_messages 0\ndetaillevel 0\n' > "$run/shots.cfg"
(cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  DOOM_SHOTS="$run/shots.reference.raw" DOOM_SHOT_TICS=$tics \
  "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
    -config "$run/shots.cfg" -timedemo "$demo" -nosound 2> /dev/null |
  grep '^shot ' > "$run/shots.tics")
{
  echo "table shots: i32 = ["
  awk '{ printf "%s%s", (NR > 1 ? ", " : "  "), $2 }' "$run/shots.tics"
  printf '\n]\n'
} > "$run/shots.ent"
rm -f "$run/shots.port"
sed "s/DEMO1/$lump/" "$here/shots_check.ent" > "$run/shots_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  run -I "$here/.." -I "$run" "$run/shots_check.ent") > "$run/shots.out"
grep -v '^shot ' "$run/shots.out"
echo "after the tics$(awk '{ printf " %s", $2 }' "$run/shots.tics"):"
python3 "$here/pictures.py" "$run/shots.reference.raw" "$run/shots.port" ${3:-0}
if grep '^shot ' "$run/shots.out" | cmp -s - "$run/shots.tics"; then
  echo "the same palettes:$(awk '{ printf " %s", $3 }' "$run/shots.tics")"
else
  echo "the palettes differ (tic and palette; the reference's <, the port's >):"
  grep '^shot ' "$run/shots.out" | diff "$run/shots.tics" - | sed 's/^/  /'
fi
