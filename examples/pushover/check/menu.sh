#!/bin/sh
# The menus gone through without a window (menu_check.ent): from the main
# menu into what is set, then a set, its first level, the help, the
# level played to its end by the game's recording of it, and back. Says
# what each window holds. Needs reference/build.sh and reference/assets.sh to have run, and the
# compiler built (build/bin) with its LLVM (LLVM_PREFIX).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/pushover
run=$out/run
mkdir -p "$run/menu"
cp "$here/menu_check.ent" "$run/menu_check.ent"
(cd "$run" && ENT_BIN=${ENT_BIN:-$root/build/bin} python3 "$root/tools/ent" \
  build -I "$here/.." menu_check.ent -o menu_check)
# (In a folder of its own: what it marks as solved is not the player's.)
rm -rf "$run/menu/levels" "$run/menu/pushover.solved"
cp "$out/upstream/recordings/finish/original/001_a.rec" "$run/menu/record.rec"
(cd "$run/menu" && "$run/menu_check")
