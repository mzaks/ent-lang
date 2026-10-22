#!/bin/sh
# Fetches LAMMPS at the stable release the LJ melt is compared with into
# build/lj/lammps and builds it there (build/lj/lammps/build/lmp): on one
# core and with OpenMP threads (its OPENMP package, `-sf omp`), compiled
# for this machine (-O3 -march=native). Needs git, cmake and a C++
# compiler with OpenMP (CC and CXX say which: the LLVM of
# build/toolchain has its runtime). (LAMMPS is under the GPL and is not
# in this repository.)
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$root/build/lj
tag=stable_30Sep2026
mkdir -p "$out"
if [ ! -d "$out/lammps/.git" ]; then
  git clone -q --depth 1 --branch "$tag" https://github.com/lammps/lammps.git \
    "$out/lammps" || exit 1
fi
cmake -S "$out/lammps/cmake" -B "$out/lammps/build" -G Ninja \
  ${CMAKE_MAKE_PROGRAM:+-DCMAKE_MAKE_PROGRAM=$CMAKE_MAKE_PROGRAM} \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=native" \
  -DBUILD_MPI=no -DBUILD_OMP=yes -DPKG_OPENMP=yes \
  -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON || exit 1
cmake --build "$out/lammps/build" || exit 1
cp "$out/lammps/bench/in.lj" "$out/in.lj"
echo "LAMMPS is $out/lammps/build/lmp"
