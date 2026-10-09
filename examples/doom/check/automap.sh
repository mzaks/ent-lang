#!/bin/sh
# Loads the shareware WAD (after reference/fetch.sh) and E1M1, draws the
# map of it and says what it found; the picture is build/doom/run/level.ppm.
# The numbers of lumps, vertexes, sectors, sides and lines, and the hashes
# after them, are those an independent reading of the file gives. Needs
# the compiler built (build/bin) and its LLVM (LLVM_PREFIX, as for
# tools/ent).
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom

cp "$doom/wad/doom1.wad" "$doom/run/doom1.wad"
(cd "$doom/run" &&
  ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" run \
    -I "$here/.." "$here/automap_check.ent") > "$doom/run/level.port"
cat > "$doom/run/level.expected" <<'END'
wad 4196020 bytes, 1264 lumps; level lump 6
vertexes 467 2733697916 sectors 85 2163873991 sides 648 4033627395
lines 475
map scale 3909 from -50331648 -318767104: 467 lines, 2911 pixels lit, 2737625973
END
diff "$doom/run/level.expected" "$doom/run/level.port"
echo "the same: E1M1 as the WAD has it"
