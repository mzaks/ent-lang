#!/bin/sh
# The screen as the port draws it, against the reference's, pixel for
# pixel, after some tics of a demo of the WAD (after reference/fetch.sh):
#   check/shots.sh 3 0,50,100,300,600      demo3, after those tics
#   check/shots.sh e1m5_uv_max 1600,3700   a demo of reference/fetch_demos.sh
# Which of the palettes each screen is shown in is compared as well.
# BLOCKS=7 for a view of another size (3 to 11; 10 if not said).
# MELTS=2 for the screens of the melts as well, from the third melt on
# (the first melts from a screen that is not the port's).
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
# (The whole view above the status bar.)
blocks=${BLOCKS:-10}
printf "screenblocks $blocks\n" > "$run/shots.cfg"
printf 'show_messages 1\ndetaillevel 0\n' >> "$run/shots.cfg"
(cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  DOOM_SHOTS="$run/shots.reference.raw" DOOM_SHOT_TICS=$tics \
  DOOM_MELTS=${MELTS:+$run/melts.reference.raw} DOOM_MELT_SKIP=${MELTS:-0} \
  "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
    -config "$run/shots.cfg" -timedemo "$demo" -nosound 2> /dev/null |
  grep '^shot \|^melt ' > "$run/shots.both")
grep '^shot ' "$run/shots.both" > "$run/shots.tics"
grep '^melt ' "$run/shots.both" > "$run/shots.melts"
{
  echo "table shots: i32 = ["
  awk '{ printf "%s%s", (NR > 1 ? ", " : "  "), $2 }' "$run/shots.tics"
  printf '\n]\n'
  # (By how many steps each screen of a melt goes on; 0 after the last.)
  echo "const MELT_SKIP = ${MELTS:-0}"
  echo "table melts: i32 = ["
  awk '{ printf "  %s,", $3 } END { print "  0" }' "$run/shots.melts"
  echo "]"
} > "$run/shots.ent"
rm -f "$run/shots.port" "$run/shots.melt"
sed "s/DEMO1/$lump/; s/View.blocks = 10/View.blocks = $blocks/" "$here/shots_check.ent" > "$run/shots_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  run -I "$here/.." -I "$run" "$run/shots_check.ent") > "$run/shots.out"
grep -v '^shot \|^melt ' "$run/shots.out"
echo "after the tics$(awk '{ printf " %s", $2 }' "$run/shots.tics"):"
python3 "$here/pictures.py" "$run/shots.reference.raw" "$run/shots.port" ${3:-0}
if grep '^shot ' "$run/shots.out" | cmp -s - "$run/shots.tics"; then
  echo "the same palettes:$(awk '{ printf " %s", $3 }' "$run/shots.tics")"
else
  echo "the palettes differ (tic and palette; the reference's <, the port's >):"
  grep '^shot ' "$run/shots.out" | diff "$run/shots.tics" - | sed 's/^/  /'
fi
if [ -n "$MELTS" ]; then
  echo "the melts' $(wc -l < "$run/shots.melts") screens:"
  python3 "$here/pictures.py" "$run/melts.reference.raw" "$run/shots.melt" 0
  grep '^melt ' "$run/shots.out" | cmp - "$run/shots.melts"
fi
