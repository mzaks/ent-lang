#!/bin/sh
# What the port is compared against: Pushover itself, as its repository
# has it at the tag 0.0.5. Of it are built its levels, dominoes and ant
# (nothing of its drawing or sound) with a program that plays a recording
# and writes a line for each tick (stubs/trace.cpp); its recordings of
# solved levels are what is played. All of it goes to build/pushover,
# which is not committed.
#
#   examples/pushover/reference/build.sh
#
# Needs git and a C++ compiler.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/pushover
game=$out/upstream

if [ ! -d "$game" ]; then
  mkdir -p "$out"
  GIT_TERMINAL_PROMPT=0 git clone -q --branch 0.0.5 \
    https://github.com/pushover/pushover.github.io.git "$game"
fi

rm -rf "$out/ref"
mkdir -p "$out/ref/src"
for name in leveldata.h leveldata.cpp levelplayer.h levelplayer.cpp ant.h \
    ant.cpp textsections.h textsections.cpp sha1.h sha1.cpp recorder.h \
    recorder.cpp tools.h; do
  cp "$game/src/$name" "$out/ref/src/$name"
done
cp "$here"/stubs/* "$out/ref/src/"

# (The game's checks of where it reads stay on: no -DNDEBUG.)
(cd "$out/ref/src" && ${CXX:-c++} -std=gnu++98 -O1 -fwrapv -w -I. \
  -include cstring -include cstdio -include stdexcept \
  leveldata.cpp levelplayer.cpp ant.cpp textsections.cpp sha1.cpp \
  recorder.cpp trace.cpp -o "$out/ref/trace")

echo "built $out/ref/trace; $(find "$game/recordings" -name '*.rec' | wc -l) recordings"
