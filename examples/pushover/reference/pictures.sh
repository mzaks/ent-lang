#!/bin/sh
# The pictures of the dominoes, which the game does not keep but makes
# when it is built: every domino in every way it stands, drawn by
# POV-Ray from the game's own scene (data/sources/domino.pov), and all
# of them put into one picture (assemble.py, as the game's assembler
# does): data/dominos.png, where the game is in build/pushover (after
# reference/build.sh).
#
#   examples/pushover/reference/pictures.sh
#
# Needs POV-Ray 3.7: `povray` where commands are found, or POVRAY names
# it. (Without the right to install it for the machine:
# `pixi init build/pushover/povray && pixi add --manifest-path
# build/pushover/povray/pixi.toml povray`, which this finds.)
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/pushover
game=$out/upstream
render=$out/render
povray=${POVRAY:-$(command -v povray || true)}
[ -n "$povray" ] || povray=$out/povray/.pixi/envs/default/bin/povray

if [ ! -x "$povray" ]; then
  echo "no POV-Ray (set POVRAY)" >&2
  exit 1
fi
if [ ! -d "$game/data/sources" ]; then
  echo "no game in $game: run reference/build.sh first" >&2
  exit 1
fi

rm -rf "$render"
mkdir -p "$render/data"
cp "$game"/data/sources/*.pov "$game/data/sources/dominos.lst" "$render/"
cp -r "$game/data/sources/crashes" "$game/data/sources/explosions" "$render/"

# The scene is of POV-Ray 3.6. A 3.7 that was built to compute fast
# takes a camera without an angle for one with too wide a one, and
# stops. With `angle 0` it asks for none; it then makes `up` and `right`
# as long as the way to `look_at` is in `direction`s, so `direction` is
# given that length, and they stay as the scene has them: the picture is
# the same.
python3 -I - "$render/domino.pov" <<'PATCH'
import sys
scene = open(sys.argv[1]).read()
near = "    look_at  <0, (H+1.5*3)/2, 0>\n"
far = "    look_at  <0, (H+1.5*3)/2, -1>\n"
assert near in scene and far in scene and "  orthographic\n" in scene
scene = scene.replace(near, near + "    direction <0, 0, 0.0001>\n")
scene = scene.replace(far, far + "    direction <0, 0, 1.1>\n")
scene = scene.replace("  orthographic\n", "  orthographic\n  angle 0\n")
open(sys.argv[1], "w").write(scene)
PATCH

# The frames the list names, as domino.ini asks for them (165 by 55,
# see-through around the domino, smoothed), many at once.
(cd "$render" && grep -v '^#' dominos.lst | awk '$4 ~ /^data\// { print $4 }' |
  sed 's|data/domino0*\([0-9]*\)\.png|\1|; s/^$/0/' | sort -un |
  xargs -P "$(nproc 2> /dev/null || echo 4)" -I{} sh -c \
    'n=$(printf %04d {}); "$0" +Idomino.pov +Odata/domino$n.png +W165 +H55 +K{} -D +UA +FN +A +R5 +MV3.6 -GA > /dev/null 2>&1 || echo "frame {} failed" >&2' \
    "$povray")

(cd "$render" && python3 -I "$here/assemble.py" "$game/data/dominos.png" 58 2 200 dominos.lst)
