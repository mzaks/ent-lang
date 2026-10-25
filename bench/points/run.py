#!/usr/bin/env python3
"""The k nearest neighbours of every point (bench/points/knn2.ent and
knn3.ent, by the grid index and by a Morton one) against the Problem Based Benchmark Suite's
nearestNeighbors (github.com/cmuparlay/pbbsbench) and its octTree: a
kd-tree split by the Morton order of the points (Blelloch and Dobson,
ALENEX 2022), on the same points.

PBBS is fetched at a fixed commit and built into build/bench/points/pbbsbench
with the machine's own instruction set; its generator (randPoints) makes
the points of its tests (benchmarks/nearestNeighbors/bench/testInputs):

  2DinCube   k 1     2Dkuzmin   k 1 (clustered)
  3DinCube   k 1     3DonSphere k 1
  3DinCube   k 10    3Dplummer  k 10 (clustered)

--size points each (PBBS's own are 10M), the clustered ones --clustered:
the grid, a cell for about two points over the box around all of them,
takes hours there at a million (most points in a few cells). Every ent
run is held against a scan by bench/points/check.cc (all k of --samples
points), and octTree's once (PBBS leaves its last point out).

The Morton index is `morton (..) leaf 32 clustered`: its points kept in
its order, as octTree asks its points in the order of its tree.

Variants, each its own process, round-robin: ent and octTree on one core
(CPU 2, taskset; PARLAY_NUM_THREADS=1) and on all (whatever ent-lang and
ParlayLib take). A run of ent makes the index anew and asks every point;
octTree's makes its tree and asks every point. Reports the median over
--rounds of each process's best run, in ms, and of ent's the making of
the index.

Usage: bench/points/run.py [--size 1000000] [--clustered 100000]
                           [--rounds 3] [--samples 200]
                           [--indexes grid,morton]
"""

import argparse
import os
import re
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "bench", "points")
PBBS = os.path.join(OUT, "pbbsbench")
PBBS_URL = "https://github.com/cmuparlay/pbbsbench.git"
PBBS_COMMIT = "3932578"
ENT = os.path.join(ROOT, "tools", "ent")
FLAGS = "CCFLAGS=-mcx16 -O3 -march=native -std=c++17 -DNDEBUG -I ."
NN = os.path.join(PBBS, "benchmarks", "nearestNeighbors")

# PBBS's tests: name, randPoints' flags, sides, k, clustered.
TESTS = [
    ("2DinCube", [], 2, 1, False),
    ("2Dkuzmin", ["-k"], 2, 1, True),
    ("3DinCube", [], 3, 1, False),
    ("3DonSphere", ["-S"], 3, 1, False),
    ("3DinCube", [], 3, 10, False),
    ("3Dplummer", ["-p"], 3, 10, True),
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def fetch_pbbs():
    if not os.path.isdir(PBBS):
        os.makedirs(OUT, exist_ok=True)
        run(["git", "clone", "-q", PBBS_URL, PBBS])
        run(["git", "-C", PBBS, "checkout", "-q", PBBS_COMMIT])
        run(["git", "-C", PBBS, "submodule", "update", "-q", "--init", "parlaylib"])
    for folder, target in (("testData/geometryData", "randPoints"),
                           ("benchmarks/nearestNeighbors/octTree", "neighbors")):
        if not os.path.exists(os.path.join(PBBS, folder, target)):
            run(["make", "-s", "-C", os.path.join(PBBS, folder), target, FLAGS])
    check = os.path.join(OUT, "check")
    if not os.path.exists(check):
        run(["g++", "-O2", "-std=c++17", os.path.join(HERE, "check.cc"), "-o", check])
    return check


def make_points(name, flags, dims, n):
    folder = os.path.join(OUT, f"{name}_{n}")
    os.makedirs(folder, exist_ok=True)
    points = os.path.join(folder, "points.pts")
    if not os.path.exists(points):
        run([os.path.join(PBBS, "testData", "geometryData", "randPoints"),
             "-d", str(dims)] + flags + [str(n), points])
    return folder


# The index of each variant: the line knn2.ent and knn3.ent declare it
# with, and what takes its place.
GRID = "grid ({sides}) cell G.cell"
INDEXES = {"grid": GRID, "morton": "morton ({sides}) leaf 32 clustered"}


def build_ent(size):
    """knn2.ent and knn3.ent for each k and index, on one core and all,
    with room for `size` points."""
    env = dict(os.environ, ENT_CFLAGS="-march=native")
    room = 1 << max(20, (size - 1).bit_length())
    binaries = {}
    for dims, k in sorted({(t[2], t[3]) for t in TESTS}):
        text = open(os.path.join(HERE, f"knn{dims}.ent")).read()
        text = text.replace("G.k = 10", f"G.k = {k}")
        sides = ", ".join("xyz"[:dims])
        cloud = open(os.path.join(HERE, "cloud.ent")).read()
        cloud = cloud.replace("capacity 1048576", f"capacity {room}")
        cloud = cloud.replace("capacity 10485760", f"capacity {room * 10}")
        open(os.path.join(OUT, "cloud.ent"), "w").write(cloud)
        for index, line in INDEXES.items():
            name = f"knn{dims}_k{k}_{index}"
            program = os.path.join(OUT, name + ".ent")
            grid = GRID.format(sides=sides)
            assert grid in text
            open(program, "w").write(text.replace(grid, line.format(sides=sides)))
            for par in (False, True):
                out = os.path.join(OUT, name + ("-par" if par else ""))
                run([ENT, "build", "-I", OUT] + (["--parallel"] if par else []) +
                    [program, "-o", out], env=env)
                binaries[(dims, k, index, par)] = out
    return binaries


def ent_times(output):
    runs = [(float(a), float(b)) for a, b in
            re.findall(r"knn ([0-9.]+) ms, made ([0-9.]+) ms", output)]
    return min(runs)


def pbbs_time(output):
    return min(float(t) for t in re.findall(r"Parlay time: ([0-9.]+)", output)) * 1000.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=1000000)
    parser.add_argument("--clustered", type=int, default=100000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--samples", type=int, default=200)
    parser.add_argument("--indexes", default=",".join(INDEXES))
    args = parser.parse_args()
    for index in set(INDEXES) - set(args.indexes.split(",")):
        del INDEXES[index]

    check = fetch_pbbs()
    octtree = os.path.join(NN, "octTree", "neighbors")
    binaries = build_ent(max(args.size, args.clustered))
    one = ["taskset", "-c", "2"]
    tests = []
    for name, flags, dims, k, clustered in TESTS:
        n = args.clustered if clustered else args.size
        tests.append((name, dims, k, n, make_points(name, flags, dims, n)))

    results = {}
    for round_ in range(args.rounds):
        for name, dims, k, n, folder in tests:
            key = (name, k)
            for par in (False, True):
                for index in INDEXES:
                    out = run(([] if par else one) + [binaries[(dims, k, index, par)]],
                              cwd=folder)
                    results.setdefault(key + (index, par), []).append(ent_times(out))
                    if round_ == 0:
                        said = subprocess.run([check, "points.pts", "found.out", str(k),
                                               str(args.samples)], cwd=folder,
                                              capture_output=True, text=True)
                        if said.returncode:
                            raise SystemExit(f"{index} {name} k {k}: {said.stdout}")
                env = dict(os.environ)
                if not par:
                    env["PARLAY_NUM_THREADS"] = "1"
                cmd = [octtree, "-d", str(dims), "-k", str(k), "-r", "3"]
                if round_ == 0 and not par:
                    cmd += ["-o", "oct.out"]
                out = run(([] if par else one) + cmd + ["points.pts"], cwd=folder, env=env)
                results.setdefault(key + ("oct", par), []).append(pbbs_time(out))
                if round_ == 0 and not par:
                    said = subprocess.run([check, "points.pts", "oct.out", str(k),
                                           str(args.samples), "but-last"], cwd=folder,
                                          capture_output=True, text=True)
                    if said.returncode:
                        raise SystemExit(f"octTree {name} k {k}: {said.stdout}")

    for par in (False, True):
        print("all cores" if par else "one core")
        print(f"{'points':<12} {'n':>9} {'k':>3} | {'grid':>9} {'(made)':>8} "
              f"| {'morton':>9} {'(made)':>8} | {'octTree':>9}")
        for name, dims, k, n, folder in tests:
            row = []
            for index in ("grid", "morton"):
                ent = results.get((name, k, index, par), [(0.0, 0.0)])
                row += [statistics.median(t for t, _ in ent),
                        statistics.median(m for _, m in ent)]
            row.append(statistics.median(results[(name, k, "oct", par)]))
            print(f"{name:<12} {n:>9} {k:>3} | {row[0]:9.2f} {row[1]:8.2f} "
                  f"| {row[2]:9.2f} {row[3]:8.2f} | {row[4]:9.2f}")


if __name__ == "__main__":
    main()
