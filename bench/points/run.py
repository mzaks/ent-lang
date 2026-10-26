#!/usr/bin/env python3
"""The k nearest neighbours of every point (bench/points/knn2.ent and
knn3.ent, by the grid index, a Morton one and a k-d tree) against the
Problem Based Benchmark Suite's nearestNeighbors
(github.com/cmuparlay/pbbsbench) and its octTree: a kd-tree split by the
Morton order of the points (Blelloch and Dobson, ALENEX 2022); and
against nanoflann (github.com/jlblancoc/nanoflann, a k-d tree of splits
at the middle of the widest spread, bench/points/nanoflann.cc), on the
same points.

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
its order, as octTree asks its points in the order of its tree; the k-d
tree `kdtree (..) leaf 16 clustered` (16 the best of 8, 16 and 32 on
3DinCube k 10). nanoflann is fetched at a fixed tag into
build/bench/points/nanoflann-src, its tree of leaves of 10 (its own
default), made on as many threads as it asks.

Variants, each its own process, round-robin: ent, octTree and nanoflann
on one core (CPU 2, taskset; PARLAY_NUM_THREADS=1, OMP_NUM_THREADS=1) and
on all (whatever ent-lang, ParlayLib and OpenMP take). A run of ent makes the index anew and asks every point;
octTree's makes its tree and asks every point. Reports the median over
--rounds of each process's best run, in ms, and of ent's the making of
the index.

--gpu adds a third: `knn` a `gpu system` (built --gpu --parallel), the
index made on the GPU (EntIndexes chooses so: only gpu systems read it),
its making timed by a `gpu system` that reads it before the CPU takes
the time; the Morton index not clustered (a clustered one is made on the
CPU). Held against octTree on all cores.

Usage: bench/points/run.py [--size 1000000] [--clustered 100000]
                           [--rounds 3] [--samples 200]
                           [--indexes grid,morton,kdtree] [--gpu]
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
NANOFLANN = os.path.join(OUT, "nanoflann-src")
NANOFLANN_URL = "https://github.com/jlblancoc/nanoflann.git"
NANOFLANN_TAG = "2.0.1"
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
    if not os.path.isdir(NANOFLANN):
        run(["git", "clone", "-q", NANOFLANN_URL, NANOFLANN])
        run(["git", "-C", NANOFLANN, "checkout", "-q", NANOFLANN_TAG])
    flann = os.path.join(OUT, "nanoflann")
    if not os.path.exists(flann):
        run(["g++", "-O3", "-march=native", "-fopenmp", "-std=c++17", "-DNDEBUG",
             "-I", os.path.join(NANOFLANN, "include"),
             os.path.join(HERE, "nanoflann.cc"), "-o", flann])
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
INDEXES = {"grid": GRID, "morton": "morton ({sides}) leaf 32 clustered",
           "kdtree": "kdtree ({sides}) leaf 16 clustered"}


# What makes knn2.ent and knn3.ent run on the GPU (see --gpu).
ON_GPU = [
    ("system made() { G.made = now() - G.started + 0.0 * len(Cells) as f64 }",
     "gpu system touch() { G.made = 0.0 * len(Cells) as f64 }\n"
     "system made() { G.made = now() - G.started }"),
    ("  Cells_moved()\n  made()", "  Cells_moved()\n  touch()\n  made()"),
    ("system knn() {", "gpu system knn() {"),
]


def build_ent(size, modes):
    """knn2.ent and knn3.ent for each k and index, on one core and all
    (and on the GPU, mode "gpu"), with room for `size` points."""
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
            for par in modes:
                source, flags = program, (["--parallel"] if par else [])
                if par == "gpu":
                    gpu_text = text.replace(
                        grid, line.format(sides=sides).replace(" clustered", ""))
                    for old, new in ON_GPU:
                        assert old in gpu_text
                        gpu_text = gpu_text.replace(old, new)
                    source = os.path.join(OUT, name + "_gpu.ent")
                    open(source, "w").write(gpu_text)
                    flags = ["--gpu", "--parallel"]
                suffix = {False: "", True: "-par", "gpu": "-gpu"}[par]
                out = os.path.join(OUT, name + suffix)
                run([ENT, "build", "-I", OUT] + flags + [source, "-o", out], env=env)
                binaries[(dims, k, index, par)] = out
    return binaries


def ent_times(output):
    runs = [(float(a), float(b)) for a, b in
            re.findall(r"knn ([0-9.]+) ms, made ([0-9.]+) ms", output)]
    return min(runs)


def flann_times(output):
    return min((float(a), float(b)) for a, b in
               re.findall(r"nanoflann ([0-9.]+) ms, made ([0-9.]+) ms", output))


def pbbs_time(output):
    return min(float(t) for t in re.findall(r"Parlay time: ([0-9.]+)", output)) * 1000.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=1000000)
    parser.add_argument("--clustered", type=int, default=100000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--samples", type=int, default=200)
    parser.add_argument("--indexes", default=",".join(INDEXES))
    parser.add_argument("--gpu", action="store_true")
    args = parser.parse_args()
    modes = [False, True] + (["gpu"] if args.gpu else [])
    for index in set(INDEXES) - set(args.indexes.split(",")):
        del INDEXES[index]

    check = fetch_pbbs()
    octtree = os.path.join(NN, "octTree", "neighbors")
    binaries = build_ent(max(args.size, args.clustered), modes)
    one = ["taskset", "-c", "2"]
    tests = []
    for name, flags, dims, k, clustered in TESTS:
        n = args.clustered if clustered else args.size
        tests.append((name, dims, k, n, make_points(name, flags, dims, n)))

    results = {}
    for round_ in range(args.rounds):
        for name, dims, k, n, folder in tests:
            key = (name, k)
            for par in modes:
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
                if par == "gpu":
                    continue
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
                env = dict(os.environ)
                if not par:
                    env["OMP_NUM_THREADS"] = "1"
                out = run(([] if par else one) + [os.path.join(OUT, "nanoflann"),
                                                  "points.pts", str(k), "3"],
                          cwd=folder, env=env)
                results.setdefault(key + ("flann", par), []).append(flann_times(out))
                if round_ == 0:
                    said = subprocess.run([check, "points.pts", "found.out", str(k),
                                           str(args.samples)], cwd=folder,
                                          capture_output=True, text=True)
                    if said.returncode:
                        raise SystemExit(f"nanoflann {name} k {k}: {said.stdout}")

    for par in modes:
        print({False: "one core", True: "all cores",
               "gpu": "GPU (octTree and nanoflann on all cores)"}[par])
        columns = list(INDEXES) + ["nanoflann"]
        print(f"{'points':<12} {'n':>9} {'k':>3} | " +
              " | ".join(f"{c:>9} {'(made)':>8}" for c in columns) +
              f" | {'octTree':>9}")
        for name, dims, k, n, folder in tests:
            cells = []
            for index in columns:
                ran = results.get((name, k, index, par) if index != "nanoflann"
                                  else (name, k, "flann", bool(par)), [(0.0, 0.0)])
                cells.append(f"{statistics.median(t for t, _ in ran):9.2f} "
                             f"{statistics.median(m for _, m in ran):8.2f}")
            oct = statistics.median(results[(name, k, "oct", bool(par))])
            print(f"{name:<12} {n:>9} {k:>3} | " + " | ".join(cells) +
                  f" | {oct:9.2f}")


if __name__ == "__main__":
    main()
