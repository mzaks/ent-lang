#!/bin/sh
# Fetches the problems of bench/sat/sample.txt from the Global Benchmark
# Database into build/sat/comp/cnf, unpacked (<hash>.cnf), and builds the
# solvers entsat is held against into build/sat/tools: MiniSat 2.2 (with
# the two fixes today's compilers need) and CaDiCaL.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/build/sat/comp/cnf"
tools="$root/build/sat/tools"
mkdir -p "$out" "$tools"
cut -d' ' -f1 "$root/bench/sat/sample.txt" | while read -r hash; do
  [ -f "$out/$hash.cnf" ] || echo "$hash"
done | xargs -r -P 8 -I{} sh -c \
  'curl -fsSL "https://benchmark-database.de/file/{}" | xz -d > "'"$out"'/{}.part" && mv "'"$out"'/{}.part" "'"$out"'/{}.cnf"'
if [ ! -x "$tools/minisat/build/release/bin/minisat" ]; then
  rm -rf "$tools/minisat"
  git clone -q --depth 1 https://github.com/niklasso/minisat.git "$tools/minisat"
  cd "$tools/minisat"
  sed -i 's/friend Lit mkLit(Var var, bool sign = false);/friend Lit mkLit(Var var, bool sign);/' minisat/core/SolverTypes.h
  sed -i 's/^inline  Lit  mkLit     (Var var, bool sign) {/inline  Lit  mkLit     (Var var, bool sign = false) {/' minisat/core/SolverTypes.h
  sed -i 's/MINISAT_LDFLAGS += --static/MINISAT_LDFLAGS +=/' Makefile
  make -s r > /dev/null
fi
if [ ! -x "$tools/cadical/build/cadical" ]; then
  rm -rf "$tools/cadical"
  git clone -q --depth 1 https://github.com/arminbiere/cadical.git "$tools/cadical"
  cd "$tools/cadical" && ./configure > /dev/null && make -s > /dev/null
fi
