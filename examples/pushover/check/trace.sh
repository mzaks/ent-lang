#!/bin/sh
# The game's recordings played by the port, tick by tick against the
# reference (after reference/build.sh): says for each how many ticks are
# the same and what differs first, and at the end how many agree all
# through. Needs the compiler built (build/bin) and its LLVM
# (LLVM_PREFIX, as for tools/ent).
#
#   examples/pushover/check/trace.sh                 all the recordings
#   examples/pushover/check/trace.sh 001_a 010       those whose names
#                                                    have one of these
#   KEEP=1 ...                                       leaves the two
#                                                    traces of the last
#                                                    recording that
#                                                    differed
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/pushover
run=$out/run
mkdir -p "$run"

cp "$here/trace_check.ent" "$run/trace_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  build -I "$here/.." trace_check.ent -o trace_check) || exit 1

same=0
other=0
for recording in "$out"/upstream/recordings/*/*.rec "$out"/upstream/recordings/*/*/*.rec; do
  [ -f "$recording" ] || continue
  name=${recording#"$out"/upstream/recordings/}
  if [ $# -gt 0 ]; then
    wanted=
    for part in "$@"; do
      case $name in *"$part"*) wanted=1 ;; esac
    done
    [ -n "$wanted" ] || continue
  fi
  (cd "$out" && ./ref/trace upstream/levels "$recording") > "$run/reference.out" 2> /dev/null
  level=$(sed -n '1s/^level //p' "$run/reference.out")
  sed 1d "$run/reference.out" > "$run/reference.trace"
  cp "$out/$level" "$run/level.level"
  cp "$recording" "$run/record.rec"
  (cd "$run" && ./trace_check) 2> "$run/port.err" |
    awk '/^ / { printf "%s", $0; next } { if (NR > 1) print ""; printf "%s", $0 } END { print "" }' \
      > "$run/port.trace"
  if cmp -s "$run/reference.trace" "$run/port.trace"; then
    same=$((same + 1))
  else
    other=$((other + 1))
    at=$(cmp "$run/reference.trace" "$run/port.trace" | sed 's/.* line //')
    echo "$name: differs at line $at of $(wc -l < "$run/reference.trace")"
    if [ $# -gt 0 ]; then
      sed -n "${at}p" "$run/reference.trace" | cut -c1-600 | sed 's/^/  reference: /'
      sed -n "${at}p" "$run/port.trace" | cut -c1-600 | sed 's/^/  port:      /'
      head -3 "$run/port.err"
    fi
    [ -n "$KEEP" ] && cp "$run/reference.trace" "$run/kept.reference" &&
      cp "$run/port.trace" "$run/kept.port"
  fi
done
echo "$same the same, $other not"
