#!/usr/bin/env python3
"""Layout and stream-count experiments (see frame.c and streams.c).

Every variant is its own binary and every measurement its own process,
round-robin over sizes and variants. Prints medians over --rounds processes
with the spread (max-min)/median, and checks that all layouts agree on the
checksum.

Usage: bench/layout/run.py [frame|streams] [--rounds 5] [--threads 12]
"""

import argparse
import os
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
OUT = os.path.join(ROOT, "build", "bench", "layout")
OPENMP = ["-fopenmp", f"-L{LLVM}/lib", f"-Wl,-rpath,{LLVM}/lib"]


def build(source, name, flags):
    exe = os.path.join(OUT, name)
    subprocess.run([f"{LLVM}/bin/clang", "-O2", "-ffp-contract=off", *flags,
                    os.path.join(HERE, source), "-o", exe], check=True)
    return exe


def measure(exes, sizes, rounds, reps, target_ms, env, key):
    results = {(n, v): [] for n in sizes for v in exes}
    checks = {}
    for _ in range(rounds):
        for n in sizes:
            for name, exe in exes.items():
                out = subprocess.run([exe, str(n), str(reps), str(target_ms)],
                                     check=True, capture_output=True,
                                     text=True, env=env).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results[(n, name)].append(float(fields[key]))
                if "checksum" in fields:
                    checks.setdefault(n, set()).add(fields["checksum"])
    for n, values in checks.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at n={n}: {values}")
    return results


def table(title, results, sizes, names, fmt, label):
    print(f"\n{title}\n")
    print(f"| variant | " + " | ".join(label(n) for n in sizes) + " |")
    print("|" + "---|" * (len(sizes) + 1))
    for name in names:
        cells = []
        for n in sizes:
            values = results[(n, name)]
            med = statistics.median(values)
            cells.append(f"{fmt(med)} ({(max(values) - min(values)) / med:.0%})")
        print(f"| {name} | " + " | ".join(cells) + " |")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("experiment", choices=["frame", "streams"])
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--threads", type=int, default=12)
    parser.add_argument("--target-ms", type=int, default=200)
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    env = dict(os.environ, OMP_NUM_THREADS=str(args.threads))

    if args.experiment == "frame":
        layouts = {"soa": ["-DLAYOUT=0"],
                   "soa-stagger": ["-DLAYOUT=0", "-DSTAGGER"],
                   "aos": ["-DLAYOUT=1"],
                   "aosoa8": ["-DLAYOUT=2", "-DW=8"],
                   "aosoa16": ["-DLAYOUT=2", "-DW=16"]}
        exes = {}
        for name, flags in layouts.items():
            exes[name] = build("frame.c", name, flags)
            exes[name + "-omp"] = build("frame.c", name + "-omp",
                                        flags + OPENMP)
        sizes = [1000, 10000, 100000, 1000000, 10000000]
        res = measure(exes, sizes, args.rounds, args.reps, args.target_ms,
                      env, "ns_per_frame")
        table(f"ns per frame, median of {args.rounds} processes; "
              f"-omp: {args.threads} threads", res, sizes, list(exes),
              lambda v: f"{v:,.0f}", lambda n: f"n={n:.0e}")
    else:
        exes = {}
        for k in [1, 2, 3, 4, 5, 8, 16]:
            exes[f"K={k}"] = build("streams.c", f"k{k}", [f"-DK={k}"])
            exes[f"K={k} omp"] = build("streams.c", f"k{k}-omp",
                                       [f"-DK={k}"] + OPENMP)
            exes[f"K={k} stagger"] = build("streams.c", f"k{k}-stagger",
                                           [f"-DK={k}", "-DSTAGGER"])
        # Total floats: 1 MB (L2) and 160 MB (DRAM), split over K streams.
        sizes = [250000, 40000000]
        res = measure(exes, sizes, args.rounds, args.reps, args.target_ms,
                      env, "gb_per_s")
        table(f"GB/s moved (read + write), median of {args.rounds} "
              f"processes; omp: {args.threads} threads", res, sizes,
              list(exes), lambda v: f"{v:,.0f}",
              lambda n: f"{n * 4 / 1e6:g} MB total")


if __name__ == "__main__":
    main()
