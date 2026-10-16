#!/bin/sh
# Fetches what entsat is checked against, into build/sat: the random
# 3-SAT sets of SATLIB (uf: satisfiable, uuf: unsatisfiable) and
# drat-trim, which checks the proofs of the unsatisfiable ones.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/build/sat"
mkdir -p "$out/cnf" "$out/tools"
satlib=https://www.cs.ubc.ca/~hoos/SATLIB/Benchmarks/SAT/RND3SAT
for set in uf50-218 uuf50-218 uf75-325 uuf75-325 uf100-430 uuf100-430 \
           uf125-538 uuf125-538 uf150-645 uuf150-645 uf175-753 uuf175-753 \
           uf200-860 uuf200-860 uf225-960 uuf225-960 uf250-1065 uuf250-1065; do
  if [ ! -d "$out/cnf/$set" ]; then
    mkdir -p "$out/cnf/$set"
    curl -fsSL "$satlib/$set.tar.gz" | tar -xz -C "$out/cnf/$set"
  fi
done
if [ ! -x "$out/tools/drat-trim" ]; then
  curl -fsSL https://raw.githubusercontent.com/marijnheule/drat-trim/master/drat-trim.c \
    -o "$out/tools/drat-trim.c"
  cc -O2 -o "$out/tools/drat-trim" "$out/tools/drat-trim.c"
fi
