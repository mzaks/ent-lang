#!/bin/sh
# Makes the environment with Mojo that examples/mojo builds its device in
# (build/mojo, by pixi, from Modular's stable channel). Once.
set -e
pixi=$(command -v pixi || echo "$HOME/.pixi/bin/pixi")
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$root/build"
cd "$root/build"
[ -f mojo/pixi.toml ] ||
  "$pixi" init mojo -c https://conda.modular.com/max/ -c conda-forge
cd mojo
"$pixi" add mojo
"$pixi" run mojo --version
