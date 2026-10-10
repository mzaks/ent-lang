#!/bin/sh
# The three demos of the shareware WAD played by the port, tic by tic
# against the reference's traces (after reference/fetch.sh demo1 demo2
# demo3): as they are, and without their monsters, of which only where
# the player is and the player's health are compared (the player shoots
# early, and what shots leave is not there yet). Says how many tics are
# the same and what differs first. Needs the compiler built (build/bin)
# and its LLVM (LLVM_PREFIX, as for tools/ent).
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run
tics=${1:-100000}

cp "$doom/wad/doom1.wad" "$run/doom1.wad"
for number in 1 2 3; do
  for without in false true; do
    name=demo$number
    [ $without = true ] && name=${name}nm
    if [ $without = true ]; then
      # The demo with "no monsters" set, for the reference to play.
      python3 -I - "$run/doom1.wad" "DEMO$number" "$run/$name.lmp" <<'END'
import struct, sys
wad = open(sys.argv[1], "rb").read()
count, table = struct.unpack_from("<ii", wad, 4)
for lump in range(count):
    at, size = struct.unpack_from("<ii", wad, table + lump * 16)
    if wad[table + lump * 16 + 8:table + lump * 16 + 16].rstrip(b"\0") == sys.argv[2].encode():
        demo = bytearray(wad[at:at + size])
        demo[7] = 1
        open(sys.argv[3], "wb").write(demo)
END
      (cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        DOOM_TRACE="$run/$name.trace" \
        "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
          -timedemo "$name" -nosound -nodraw > /dev/null 2>&1 || true)
    fi
    sed "s/DEMO1/DEMO$number/; s/Without = false/Without = $without/; s/Tics = 300/Tics = $tics/" \
      "$here/trace_check.ent" > "$run/trace_check.ent"
    (cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
      run -I "$here/.." "$run/trace_check.ent") > "$run/$name.out"
    grep -v '^sound ' "$run/$name.out" > "$run/$name.port"
    only=
    [ $without = true ] && only="--only x,y,z,angle,health"
    echo "$name:"
    python3 "$here/../reference/compare.py" $only "$run/$name.trace" \
      "$run/$name.port" | sed 's/^/  /' | head -6
  done
done
