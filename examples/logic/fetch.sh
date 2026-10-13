#!/bin/sh
# Fetches the circuits the logic simulator is held against, into
# build/logic/src (which is not part of the repository): the ISCAS-85 and
# ISCAS-89 netlists as Verilog, from a collection of them that says no
# licence, and the EPFL combinational suite (MIT). Each at the commit the
# simulator was checked with.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/logic/src
cd build/logic/src
fetch() {
  if [ ! -d "$1" ]; then
    git clone -q "$2" "$1"
  fi
  git -C "$1" checkout -q "$3"
}
fetch iscas https://github.com/santoshsmalagi/Benchmarks 8a0b4d2ca0e85e9d2425a96212605e5327a22c61
fetch epfl https://github.com/lsils/benchmarks 82d8cc6910419298e713a46644ed59fd3df53038
echo "circuits in build/logic/src; examples/logic/prepare.py makes them ready"
