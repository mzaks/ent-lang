#!/bin/sh
# The demos of reference/fetch_demos.sh played by the port, tic by tic
# against the reference: for each, how many tics are the same and what
# differs first (as check/trace.sh says of the WAD's own three). A demo
# is put in the place of DEMO1 in a copy of the WAD for the port to
# find. With names, only those: check/demos.sh e1m5_uv_max
# DUMP=tic: what differs of the things at that tic too; with ALL=1, of
# what the trace's hash leaves out as well (flags, the way a monster
# walks, how fast a thing moves, its floor and ceiling). TICS=n: no
# more than n tics of the port. The sounds that are heard are compared
# as well. SKILL=n: the demo as if it were played in that skill (0 to 4;
# its keys are then no good for the level, but they are the same for
# both), as the fifth is found in none of the fetched ones.
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run/demos
mkdir -p "$run"
names=$*
all=
dumpall=false
lines=thing
[ -n "$ALL" ] && all=1 && dumpall=true && lines=more
[ -z "$names" ] && names=$(cd "$doom/demos" && ls *.lmp | sed 's/\.lmp$//')
for name in $names; do
  mkdir -p "$run/$name"
  cp "$doom/demos/$name.lmp" "$run/$name/$name.lmp"
  python3 -I - "$doom/wad/doom1.wad" "$run/$name/$name.lmp" "$run/$name/doom1.wad" "$SKILL" <<'END'
import struct, sys
wad = bytearray(open(sys.argv[1], "rb").read())
demo = bytearray(open(sys.argv[2], "rb").read())
if sys.argv[4]:
    demo[1] = int(sys.argv[4])
    open(sys.argv[2], "wb").write(demo)
count, table = struct.unpack_from("<ii", wad, 4)
for lump in range(count):
    entry = table + lump * 16
    if wad[entry + 8:entry + 16].rstrip(b"\0") == b"DEMO1":
        struct.pack_into("<ii", wad, entry, len(wad), len(demo))
        break
open(sys.argv[3], "wb").write(wad + demo)
END
  (cd "$run/$name" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    DOOM_TRACE="$run/$name/trace" DOOM_SOUNDS="$run/$name/sounds" \
    DOOM_DUMP=${DUMP:--9} \
    DOOM_DUMPFILE="$run/$name/refdump" DOOM_DUMPALL=$all \
    "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
      -timedemo "$name" -nosound -nodraw > /dev/null 2>&1 || true)
  sed "s/Tics = 300/Tics = ${TICS:-100000}/; s/Dump = -5/Dump = ${DUMP:--5}/; s/Dumpall = false/Dumpall = $dumpall/" \
    "$here/trace_check.ent" > "$run/$name/check.ent"
  (cd "$run/$name" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
    run -I "$here/.." "$run/$name/check.ent" 2>&1) > "$run/$name/out"
  grep -v '^thing \|^more \|^also \|^last \|^sound ' "$run/$name/out" > "$run/$name/port"
  # (The sounds of a tic in one order for both: the port starts some
  # later in a tic than the reference: what the player picks up, and
  # what a line that is walked over starts.)
  sed -n 's/^sound //p' "$run/$name/out" | sort -s -n -k1,1 -k2,2 -k3,3 -k4,4 > "$run/$name/portsounds"
  sort -s -n -k1,1 -k2,2 -k3,3 -k4,4 "$run/$name/sounds" > "$run/$name/sounds.sorted"
  mv "$run/$name/sounds.sorted" "$run/$name/sounds"
  echo "$name ($(wc -l < "$run/$name/trace") tics):"
  python3 "$here/../reference/compare.py" "$run/$name/trace" "$run/$name/port" |
    sed 's/^/  /' | head -6
  # (The sounds that are heard, each with its tic, how loud and from
  # which side. Without TICS, which stops the port early.)
  if [ -z "$TICS" ]; then
    if cmp -s "$run/$name/sounds" "$run/$name/portsounds"; then
      echo "  the same: $(wc -l < "$run/$name/sounds") sounds"
    else
      echo "  sounds differ (the reference's <, the port's >):"
      diff "$run/$name/sounds" "$run/$name/portsounds" | sed 's/^/    /' | head -${LINES:-8}
    fi
  fi
  if [ -n "$DUMP" ]; then
    sed -n "s/^$lines //p" "$run/$name/out" > "$run/$name/portdump"
    if [ -n "$all" ]; then
      sed -n "s/^also //p" "$run/$name/out" > "$run/$name/portalso"
      paste -d' ' "$run/$name/portdump" "$run/$name/portalso" > "$run/$name/portboth"
      mv "$run/$name/portboth" "$run/$name/portdump"
      sed -n "s/^last //p" "$run/$name/out" >> "$run/$name/portdump"
    fi
    diff "$run/$name/refdump" "$run/$name/portdump" | head -${LINES:-20}
  fi
done
