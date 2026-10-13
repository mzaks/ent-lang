#!/bin/sh
# Fetches the reference, OpenABM-Covid19, at the commit this model was
# held against, into build/epidemic/reference, and copies its household
# panel to build/epidemic/run, where the checks start their programs.
# (Its files are under the GPL and are not in this repository.)
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/epidemic
commit=367a0d3c9c8ce9c73c0b872537c06c71a9a58337
mkdir -p "$out/run"
if [ ! -d "$out/reference/.git" ]; then
  git clone https://github.com/BDI-pathogens/OpenABM-Covid19.git "$out/reference" || exit 1
fi
(cd "$out/reference" && (git checkout -q "$commit" 2>/dev/null ||
  (git fetch -q origin "$commit" && git checkout -q "$commit"))) || exit 1
cp "$out/reference/tests/data/baseline_household_demographics.csv" "$out/run/" || exit 1
echo "the reference is in $out/reference"
