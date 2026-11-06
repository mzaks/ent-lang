#!/bin/sh
# The device workloads of drive.ent in ent, C, Rust and Goose: each built
# for any x86-64 and for this processor (ENT_CFLAGS=-march=native, which
# the modules follow), run on one core (pinned to $CORE) and built
# --parallel on all; each line is the best of drive.ent's rounds, with
# the sum of what it gave (the same in every language). Goose needs
# examples/goose/setup.sh, Rust cargo.
#
#   sh bench/devices/run.sh [languages]    (default: ent c rust goose)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${WORK:-$root/build/bench-devices}
core=${CORE:-2}
mkdir -p "$work"
sed '/\/\/16$/d' "$here/drive.ent" > "$work/drive_ent.ent"
for language in ${*:-ent c rust goose}; do
  driver="$here/drive.ent"
  [ "$language" = ent ] && driver="$work/drive_ent.ent"
  for cpu in any native; do
    flags=""
    [ "$cpu" = native ] && flags="-march=native"
    ENT_CFLAGS="$flags" "$root/tools/ent" build --no-cache \
      -I "$here/$language" "$driver" -o "$work/$language-$cpu.one"
    ENT_CFLAGS="$flags" "$root/tools/ent" build --no-cache --parallel \
      -I "$here/$language" "$driver" -o "$work/$language-$cpu.all"
    echo "== $language, $cpu, one core"
    taskset -c "$core" "$work/$language-$cpu.one"
    echo "== $language, $cpu, all cores"
    "$work/$language-$cpu.all"
  done
done
