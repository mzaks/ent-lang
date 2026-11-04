#!/bin/sh
# Copyright (C) 2024-2026 Martin Lange and the beecs contributors
# (github.com/mlange-42/beecs); ported to ent-lang by Maxim Zaks, 2026.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as
# published by the Free Software Foundation (examples/beecs/COPYING).
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# Fetches beecs (Go, GPL-3.0) at the commit the ent-lang model was held
# against into build/beecs/reference, builds the runner next to this
# script against it as build/beecs/ref_beecs, and copies its weather
# files (daily foraging hours, Berlin 2000 to 2006) to build/beecs/run,
# where the model is started. None of beecs's files are in this
# repository.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out="$root/build/beecs"
commit=28958a04b21591a1afafdce2ffba38eb3d5fbd9e
mkdir -p "$out" "$out/run"
if [ ! -d "$out/reference/.git" ]; then
  git clone https://github.com/mlange-42/beecs.git "$out/reference"
fi
git -C "$out/reference" fetch -q origin
git -C "$out/reference" checkout -q "$commit"
rm -rf "$out/runner"
cp -r "$here/runner" "$out/runner"
cd "$out/runner"
go mod edit -replace "github.com/mlange-42/beecs=$out/reference"
go mod tidy
go build -o "$out/ref_beecs" .
cp -r "$out/reference/data/foraging-period" "$out/run/"
echo "reference: $out/ref_beecs, weather: $out/run/foraging-period"
