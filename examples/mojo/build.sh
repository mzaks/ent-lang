#!/bin/sh
# Builds a module's Mojo, `$1.mojo`, into the object tools/ent asks for
# (ENT_OUTPUT), and says in ENT_LINK what it needs linked: the Mojo
# runtime's library, where the Mojo it was built with keeps it. With
# `mojo` on the PATH (an environment active) that one, else the one
# setup.sh put in build/mojo.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if command -v mojo > /dev/null; then
  run=""
else
  pixi=$(command -v pixi || echo "$HOME/.pixi/bin/pixi")
  run="$pixi run --manifest-path $root/build/mojo/pixi.toml"
fi
$run mojo build --emit object "$1.mojo" -o "$ENT_OUTPUT"
prefix=$($run sh -c 'echo "${CONDA_PREFIX:-$(dirname "$(dirname "$(command -v mojo)")")}"')
echo "-L$prefix/lib -lKGENCompilerRTShared -Wl,-rpath,$prefix/lib" > "$ENT_LINK"
