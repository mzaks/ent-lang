#!/bin/sh
# A saved world against the reference (after check/trace.sh, which leaves
# the reference's traces of the WAD's demos in build/doom/run):
#   check/save.sh 3 500      demo 3, saved before its tic 500
# The demo is played to its end with the world saved on the way, which
# must change nothing; then another run reads the saved world and plays
# on from there, and must write what the reference writes from that tic
# to the demo's end. Needs the compiler built (build/bin) and its LLVM
# (LLVM_PREFIX, as for tools/ent).
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
run=$root/build/doom/run
number=${1:-3}
at=${2:-500}
cp "$root/build/doom/wad/doom1.wad" "$run/doom1.wad"
rm -f "$run/check.sav"
for load in false true; do
  sed "s/DEMO1/DEMO$number/; s/Tics = 300/Tics = 100000/; s/Load = false/Load = $load/; s/SaveAt = 500/SaveAt = $at/" \
    "$here/save_check.ent" > "$run/save_check.ent"
  (cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
    run -I "$here/.." "$run/save_check.ent") > "$run/save.$load.out"
  grep '^saved ' "$run/save.$load.out" | sed "s/^/$( [ $load = true ] && echo loaded || echo saving ): /"
  grep -v '^sound \|^saved ' "$run/save.$load.out" > "$run/save.$load.port"
done
echo "the file: $(wc -c < "$run/check.sav") bytes"
echo "played through, saving before tic $at:"
python3 "$here/../reference/compare.py" "$run/demo$number.trace" \
  "$run/save.false.port" | sed 's/^/  /' | head -6
echo "loaded and played on:"
awk -v at="$at" '$3 >= at' "$run/demo$number.trace" > "$run/save.trace"
python3 "$here/../reference/compare.py" "$run/save.trace" \
  "$run/save.true.port" | sed 's/^/  /' | head -6
