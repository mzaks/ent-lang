#!/bin/sh
# Builds a module's Goose, `$1.goose`, into the object tools/ent asks for
# (ENT_OUTPUT): `goose` makes it into C (with the runtime, --standalone),
# and $1_shim.c, which includes that, is compiled by ENT_CC. With
# ENT_EMIT=llvm ($1.emit) the same as LLVM IR instead, by ENT_CLANG (the
# clang of the LLVM that reads it), which tools/ent reads to check its
# extern fns. `goose` is $GOOSE, else the one on the PATH, else the one
# setup.sh built in build/goose. ENT_CPU says what processor it is made
# for, as the program is (-march=native, say). (A copy of the Goose is compiled, in a folder of its
# own: next to it goose would read $1.build, ent's, as a project file.)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
goose=${GOOSE:-$(command -v goose || echo "$root/build/goose/build/goose")}
work=$(dirname "$ENT_OUTPUT")/goose.$1
mkdir -p "$work"
cp "$1.goose" "$work/$1.goose"
"$goose" -o "$work/$1_goose.c" --header "$work/$1_goose.h" --standalone \
  "$work/$1.goose"
if [ "$ENT_EMIT" = llvm ]; then
  "$ENT_CLANG" -O2 $ENT_CPU -S -emit-llvm -I "$ENT_INCLUDE" -I "$work" \
    "$1_shim.c" -o "$ENT_OUTPUT"
  exit 0
fi
"$ENT_CC" -O2 $ENT_CPU -I "$ENT_INCLUDE" -I "$work" -c "$1_shim.c" -o "$ENT_OUTPUT"
echo "-lm" > "$ENT_LINK"
