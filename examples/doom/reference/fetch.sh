#!/bin/sh
# What the port is compared against: Chocolate Doom, with a patch
# (trace.patch) that writes a line for each tic of a level where
# DOOM_TRACE names a file, and what its fixed point, tables and random
# numbers give where DOOM_MATHS does (see ../check/maths.sh); and the
# shareware WAD (v1.9). Both go to build/doom, which is not committed.
# Needs git, curl, cmake, a C compiler and SDL2.
#
#   examples/doom/reference/fetch.sh          fetch and build
#   examples/doom/reference/fetch.sh demo1    and play a demo: its trace is
#                                             build/doom/run/demo1.trace
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
doom=$root/build/doom
commit=895f581c5d91497bdda0516612da803fe5843e28
wad_sha1=5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
wad_url=https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad

mkdir -p "$doom/ref" "$doom/wad" "$doom/run"

if [ ! -d "$doom/ref/chocolate-doom" ]; then
  git clone -q https://github.com/chocolate-doom/chocolate-doom.git \
    "$doom/ref/chocolate-doom"
  git -C "$doom/ref/chocolate-doom" checkout -q "$commit"
  git -C "$doom/ref/chocolate-doom" apply "$here/trace.patch"
fi

if [ ! -f "$doom/wad/doom1.wad" ]; then
  curl -sL -o "$doom/wad/doom1.wad" "$wad_url"
fi
echo "$wad_sha1  $doom/wad/doom1.wad" | sha1sum -c -

if [ ! -x "$doom/ref/build/src/chocolate-doom" ]; then
  cmake -S "$doom/ref/chocolate-doom" -B "$doom/ref/build" \
    -DENABLE_SDL2_NET=Off -DENABLE_SDL2_MIXER=Off \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
  cmake --build "$doom/ref/build" --target chocolate-doom
fi

for demo in "$@"; do
  # Stops with "timed N gametics" when the demo is over.
  (cd "$doom/run" &&
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    DOOM_TRACE="$doom/run/$demo.trace" \
    "$doom/ref/build/src/chocolate-doom" -iwad "$doom/wad/doom1.wad" \
      -timedemo "$demo" -nosound -nodraw > /dev/null 2>&1 || true)
  echo "$demo: $(wc -l < "$doom/run/$demo.trace") tics"
done
