#!/bin/bash
# The examples that have a window, as web pages (ent build --web), and
# a page that lists them: what GitHub Pages shows of ent-lang.
#
#   tools/pages/build.sh out        then: python3 -m http.server -d out
#
# Needs what `ent build --web` needs: Emscripten (build/emsdk) and raylib
# built for the web (build/raylib/src). Each demo is a folder of its own
# in `out` (index.html, .js, .wasm, and .data where it has files); the
# page that lists them is tools/pages/index.html, with the pictures in
# tools/pages/shots.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:?"the folder to write the site to"}
mkdir -p "$out"
out=$(cd "$out" && pwd)

# folder  source  build flags (the folders of its modules; capacities;
# the keys a phone is given, where a demo is played or steered with keys)
demos=(
  "pacman|examples/pacman.ent|--touch-keys arrows,Enter"
  "snake|examples/snake.ent|--touch-keys arrows,Space"
  "towers|examples/towers.ent|-I examples --touch-keys Space"
  "swarm|examples/swarm/swarm.ent|-I examples --touch-keys wasd,Enter,B"
  # (Room for the grids and searches of 8 vehicles, where the program
  # has it for 96: a web page's memory is at most 4 GB.)
  "parking|examples/parking/show.ent|--capacity Cells=5120000 --capacity Field=5120000 --capacity Nodes=4800000 --capacity Heap=4800000 --capacity Best=23040000 --capacity Way=4096 --capacity Ahead=512 --touch-keys arrows,Space:brake,A:park,R:again,P:prev,N:next,V:view,H:trailer,B:assist,Shift:slow,1,2,3,4,5"
  "traffic|examples/traffic/show.ent|--touch-keys Space:hold,Up:faster,Down:slower,B:brake,1,2,3"
  "junctions|examples/traffic/junctions.ent|--touch-keys arrows,Space:hold,P:polite+,O:polite-,Q:NS+,A:NS-,W:EW+,S:EW-,1,2,3,4,5,6,7"
  "orrery|examples/orrery.ent|--touch-keys Space:hold"
  "bounce|examples/bounce.ent|--touch-keys Space:hold"
  "layout|examples/layout_demo.ent|-I examples --touch-keys Space:stack,Enter:card,I:inspect,O:hide,Z:zoom"
  "clay-sidebar|examples/clay/sidebar.ent|-I examples"
  "clay-transitions|examples/clay/transitions.ent|-I examples"
  "clay-video|examples/clay/video_demo.ent|-I examples"
)

cd "$root"
for demo in "${demos[@]}"; do
  [[ "$demo" == \#* ]] && continue
  IFS='|' read -r name source flags <<<"$demo"
  echo "$name: $source"
  mkdir -p "$out/$name"
  # shellcheck disable=SC2086
  tools/ent build --web --no-index-remarks $flags "$source" \
    -o "$out/$name/index.html" >"$out/$name.log" 2>&1 ||
    { cat "$out/$name.log"; exit 1; }
  rm "$out/$name.log"
done
cp "$here/index.html" "$out/index.html"
mkdir -p "$out/shots"
cp "$here"/shots/*.webp "$out/shots/"
# (No Jekyll: GitHub Pages serves the files as they are.)
touch "$out/.nojekyll"
echo "written: $out"
