#!/bin/sh
# The Dish Pong page in one folder (build/cl_bridge/web unless given):
# index.html, worker.mjs, pong.mjs and the dish with its game as
# WebAssembly (dish.mjs, dish.wasm). Serve the folder, e.g.
#   python3 -m http.server -d build/cl_bridge/web
# and open http://localhost:8000 (a page from file:// cannot start the worker).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${1:-$root/build/cl_bridge/web}
mkdir -p "$out"
"$root/tools/ent" build --shared --web "$here/../network.ent" "$here/../bridge.c" \
    "$here/../game.c" -o "$out/dish.mjs"
# (The page as a whole document: index.html is written without the
# skeleton, as a claude.ai artifact takes it.)
{ printf '<!doctype html>\n<html lang="en"><head><meta charset="utf-8">\n'
  printf '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
  printf '</head><body>\n'; cat "$here/index.html"; printf '\n</body></html>\n'; } > "$out/index.html"
cp "$here/worker.mjs" "$here/pong.mjs" "$out/"
echo "$out"
