#!/bin/sh
# Every thing of one tic of a demo, as the reference has them and as the
# port has them, and what differs: for finding why a trace stops being
# the same (check/trace.sh says at which tic).
#   check/dump.sh 3 205        demo3, tic 205
#   check/dump.sh 3 205 nm     the same without its monsters
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
run=$doom/run
number=$1
tic=$2
without=false
name=demo$number
[ "$3" = nm ] && without=true && name=${name}nm
demo=$name
[ $without = false ] && demo=demo$number
(cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  DOOM_TRACE="$run/$name.trace" DOOM_DUMP=$tic DOOM_DUMPFILE="$run/$name.refdump" \
  "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
    -timedemo "$demo" -nosound -nodraw > /dev/null 2>&1 || true)
sed "s/DEMO1/DEMO$number/; s/Without = false/Without = $without/; s/Tics = 300/Tics = $((tic + 1))/; s/Dump = -5/Dump = $tic/" \
  "$here/trace_check.ent" > "$run/dump_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  run -I "$here/.." "$run/dump_check.ent") | sed -n 's/^thing //p' > "$run/$name.portdump"
echo "$(wc -l < "$run/$name.refdump") things in the reference, $(wc -l < "$run/$name.portdump") in the port"
diff "$run/$name.refdump" "$run/$name.portdump" | head -${4:-20}
