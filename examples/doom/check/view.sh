#!/bin/sh
# The walls that are seen, against the reference's (after
# reference/fetch.sh): from where the player starts in E1M1, and from
# where each thing of the level is, turned another way each time. Says
# nothing but "the same" where they agree. Needs the compiler built
# (build/bin) and its LLVM (LLVM_PREFIX, as for tools/ent).
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

reference env DOOM_VIEW="$run/view.reference"
port view_check.ent > "$run/view.port"
diff "$run/view.reference" "$run/view.port"
echo "the same: $(grep -c '^wall' "$run/view.port") pieces of walls from the start"

reference env DOOM_VIEW="$run/views.reference" DOOM_VIEW_COUNT=1000
# The views the reference took, as a table for the port to take them by.
{
  echo "table views { x: i32, y: i32, z: i32, angle: u32 } = ["
  grep '^view' "$run/views.reference" |
    awk '{ printf "%s  { x: %s, y: %s, z: %s, angle: %s }", (NR > 1 ? ",\n" : ""), $2, $3, $4, $5 }'
  printf '\n]\n'
} > "$run/views.ent"
port views_check.ent > "$run/views.port"
diff "$run/views.reference" "$run/views.port"
echo "the same: $(grep -c '^wall' "$run/views.port") pieces of walls from $(grep -c '^view' "$run/views.port") places"
