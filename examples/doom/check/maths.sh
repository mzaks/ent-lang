#!/bin/sh
# The fixed point, the tables and the random numbers of the port against
# the reference's (after reference/fetch.sh): says nothing but "the same"
# where they agree. Needs the compiler built (build/bin) and its LLVM
# (LLVM_PREFIX, as for tools/ent).
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom

(cd "$doom/run" &&
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  DOOM_MATHS="$doom/run/maths.reference" \
  "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
    -nosound -nodraw > /dev/null 2>&1 || true)
ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" run \
  -I "$here/.." "$here/maths.ent" > "$doom/run/maths.port"
diff "$doom/run/maths.reference" "$doom/run/maths.port"
echo "the same: $(wc -l < "$doom/run/maths.port") lines"
