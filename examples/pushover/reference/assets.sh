#!/bin/sh
# Puts the game's files next to the port's sources (examples/pushover:
# `data`, `themes`, `levels`), where the port declares them (`asset`): so
# `ent build` takes them along, or into the program (`--embed`). They are
# not committed. Needs reference/build.sh and reference/pictures.sh to
# have run.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
game=$root/build/pushover/upstream
port=$here/..
[ -f "$game/data/dominos.png" ] || {
  echo "no $game/data/dominos.png: run reference/build.sh and reference/pictures.sh" >&2
  exit 1
}
rm -rf "$port/data" "$port/themes" "$port/levels"
mkdir -p "$port/data" "$port/themes" "$port/levels"
cp "$game"/data/*.ogg "$game"/data/*.png "$port/data/"
cp "$game"/themes/*.lua "$game"/themes/*.png "$game"/themes/*.ogg "$port/themes/"
for set in "$game"/levels/*/; do
  name=$(basename "$set")
  mkdir -p "$port/levels/$name"
  cp "$set"*.level "$port/levels/$name/"
done
