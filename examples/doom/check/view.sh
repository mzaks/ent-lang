#!/bin/sh
# What is seen of E1M1, against the reference's (after
# reference/fetch.sh): the pieces of walls from where each thing of the
# level is (the player first), turned another way each time; and the
# pictures from all those places, pixel for pixel, without what is masked (sprites and
# the textures in openings). Says nothing but "the same" where they
# agree. Needs the compiler built (build/bin) and its LLVM (LLVM_PREFIX,
# as for tools/ent).
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run

reference() {
  (cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$@" \
    "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
    -nosound -warp 1 1 -skill 3 > /dev/null 2>&1 || true)
}
port() {
  (cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
    run -I "$here/.." -I "$run" "$here/$1")
}

cp "$doom/wad/doom1.wad" "$run/doom1.wad"

reference env DOOM_VIEW="$run/views.reference" DOOM_VIEW_COUNT=1000
# The views the reference took, as a table for the port to take them by.
{
  echo "table views { x: i32, y: i32, z: i32, angle: u32 } = ["
  grep '^view' "$run/views.reference" |
    awk '{ printf "%s  { x: %s, y: %s, z: %s, angle: %s }", (NR > 1 ? ",\n" : ""), $2, $3, $4, $5 }'
  printf '\n]\n'
} > "$run/views.ent"
# (The port also writes what it worked out for each piece, which is
# looked at by hand where a picture differs: DOOM_VIEW_DETAIL makes the
# reference write the same.)
port views_check.ent | grep -E '^(view|wall)' > "$run/views.port"
diff "$run/views.reference" "$run/views.port"
echo "the same: $(grep -c '^wall' "$run/views.port") pieces of walls from $(grep -c '^view' "$run/views.port") places"

# The pictures: the reference's without what is masked, one after
# another, and the port's.
reference env DOOM_VIEW="$run/frames.reference" DOOM_VIEW_COUNT=1000 \
  DOOM_VIEW_SKIP=m
rm -f "$run/frames.port"
port frames_check.ent
cmp "$run/frames.reference.raw" "$run/frames.port"
echo "the same: $(($(wc -c < "$run/frames.port") / 64000)) pictures, pixel for pixel"
