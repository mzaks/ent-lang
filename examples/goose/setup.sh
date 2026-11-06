#!/bin/sh
# Builds the Goose compiler that examples/goose makes its device with
# (build/goose/build/goose), from Goose's repository at the commit the
# example was written against (it is young, and its C may change). Needs
# git, cmake and a C++ compiler. Once.
set -e
commit=335fddc13dc74666398ffe160ec8a8f2958329d0
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$root/build"
cd "$root/build"
[ -d goose/.git ] || git clone https://github.com/aardappel/goose goose
cd goose
git fetch --quiet origin
git checkout --quiet "$commit"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build --target goose -j "$(nproc 2>/dev/null || echo 4)"
build/goose --version 2>/dev/null || true
