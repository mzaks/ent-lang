#!/bin/sh
# Fetches the external circuits of spec section 9.3 into build/analog
# (not into the repo): the IBM power grid benchmarks (S. R. Nassif,
# "Power grid analysis benchmarks", ASP-DAC 2008), the DC ones, ibmpg1 to
# ibmpg6, each a SPICE netlist and its published solution, from the
# mirror at UC Santa Barbara. Each becomes build/analog/sources/ibmpg<k>.cir
# (prepare.py reads it from there) and build/analog/ibmpg/ibmpg<k>.solution.
#
#   examples/analog/fetch.sh            ibmpg1 to ibmpg6
#   examples/analog/fetch.sh 1 2        those
#
# Where the mirror cannot be reached it says so, and check 7 leaves the
# grids out.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MIRROR=https://web.ece.ucsb.edu/~lip/PGBenchmarks/ibmpg
GRIDS="$ROOT/build/analog/ibmpg"
SOURCES="$ROOT/build/analog/sources"
mkdir -p "$GRIDS" "$SOURCES"

[ $# -gt 0 ] || set -- 1 2 3 4 5 6
for k in "$@"; do
  name=ibmpg$k
  for file in $name.spice $name.solution; do
    if [ ! -s "$GRIDS/$file" ]; then
      if ! curl -sSfL -o "$GRIDS/$file.bz2" "$MIRROR/$file.bz2"; then
        echo "$name: $MIRROR/$file.bz2 cannot be fetched; left out"
        rm -f "$GRIDS/$file.bz2"
        continue 2
      fi
      bunzip2 -f "$GRIDS/$file.bz2"
    fi
  done
  # (A title line first, as SPICE reads it: the netlist's first line is a
  # comment, which would be taken for the title and lost.)
  { echo "IBM power grid $name (Nassif 2008), DC"; cat "$GRIDS/$name.spice"; } \
    > "$SOURCES/$name.cir"
  echo "$name: $(grep -c . "$GRIDS/$name.spice") lines"
done
