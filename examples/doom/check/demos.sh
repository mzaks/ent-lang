#!/bin/sh
# The demos of reference/fetch_demos.sh played by the port, tic by tic
# against the reference: for each, how many tics are the same and what
# differs first (as check/trace.sh says of the WAD's own three). A demo
# is put in the place of DEMO1 in a copy of the WAD for the port to
# find. With names, only those: check/demos.sh e1m5_uv_max
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run/demos
mkdir -p "$run"
names=$*
[ -z "$names" ] && names=$(cd "$doom/demos" && ls *.lmp | sed 's/\.lmp$//')
for name in $names; do
  mkdir -p "$run/$name"
  cp "$doom/demos/$name.lmp" "$run/$name/$name.lmp"
  python3 -I - "$doom/wad/doom1.wad" "$run/$name/$name.lmp" "$run/$name/doom1.wad" <<'END'
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
  (cd "$run/$name" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    DOOM_TRACE="$run/$name/trace" DOOM_DUMP=${DUMP:--9} \
    DOOM_DUMPFILE="$run/$name/refdump" \
    "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
      -timedemo "$name" -nosound -nodraw > /dev/null 2>&1 || true)
  sed "s/Tics = 300/Tics = 100000/; s/Dump = -5/Dump = ${DUMP:--5}/" \
    "$here/trace_check.ent" > "$run/$name/check.ent"
  (cd "$run/$name" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
    run -I "$here/.." "$run/$name/check.ent" 2>&1) > "$run/$name/out"
  grep -v '^thing ' "$run/$name/out" > "$run/$name/port"
  echo "$name ($(wc -l < "$run/$name/trace") tics):"
  python3 "$here/../reference/compare.py" "$run/$name/trace" "$run/$name/port" |
    sed 's/^/  /' | head -6
  if [ -n "$DUMP" ]; then
    sed -n 's/^thing //p' "$run/$name/out" > "$run/$name/portdump"
    diff "$run/$name/refdump" "$run/$name/portdump" | head -${LINES:-20}
  fi
done
