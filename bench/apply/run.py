#!/usr/bin/env python3
"""Cross-entity writes (see apply.c): ent.apply against hand-written C,
swept over the number of guns and of target ships.

Every variant is its own binary and every measurement its own process,
round-robin over configurations and variants. Reports medians over
--rounds processes with their spread, and checks that all variants agree
on the checksum.

Usage: bench/apply/run.py [--rounds 5] [--frames 50] [--reps 5]
"""

import argparse
import itertools
import os
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
# Extra compiler flags for every binary, e.g. BENCH_CFLAGS=-march=native.
EXTRA_CFLAGS = os.environ.get("BENCH_CFLAGS", "").split()
OUT = os.path.join(ROOT, "build", "bench", "apply")
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
OPENMP = [f"-I{LLVM}/include", f"-L{LLVM}/lib",
          f"-Wl,-rpath,{LLVM}/lib"]
LOWER = ["--convert-scf-to-cf", "--convert-to-llvm",
         "--reconcile-unrealized-casts"]
PARALLEL = ["--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1",
            "--convert-scf-to-openmp", "--canonicalize", "--ent-omp-nowait"]
# name: (VARIANT, program, ent-opt passes before LOWER, OpenMP)
VARIANTS = {
    "c-index": (0, None, None, False),
    "c-atomic-par": (1, None, None, True),
    "c-buffered": (2, None, None, True),
    "compiled-rows": (3, "fire.mlir", ["--ent-lower-to-loops"], False),
    "compiled-rows-par": (3, "fire.mlir", PARALLEL, True),
    "compiled-gen": (3, "fire_generational.mlir", ["--ent-lower-to-loops"],
                     False),
}


def build(name, number, program, passes, openmp):
    exe = os.path.join(OUT, name)
    extra = []
    if program:
        directory = os.path.join(OUT, name + ".d")
        os.makedirs(directory, exist_ok=True)
        source = os.path.join(HERE, program)
        subprocess.run([ENT_TRANSLATE, "--ent-to-c-header", source, "-o",
                        os.path.join(directory, "fire_world.h")], check=True)
        mlir = subprocess.run([ENT_OPT, source, *passes, *LOWER], check=True,
                              capture_output=True, text=True).stdout
        ll = os.path.join(directory, "fire.ll")
        subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                        "-o", ll], input=mlir, text=True, check=True)
        extra = [ll, "-Wno-override-module", f"-I{directory}"]
    if openmp:
        extra += ["-fopenmp", *OPENMP]
    subprocess.run([f"{LLVM}/bin/clang", "-O2", *EXTRA_CFLAGS,
                    "-ffp-contract=off",
                    f"-DVARIANT={number}", os.path.join(HERE, "apply.c"),
                    *extra, "-o", exe], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--frames", type=int, default=50)
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--guns", default="100000,1000000")
    parser.add_argument("--ships", default="16,10000,1000000")
    parser.add_argument("--csv")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)

    exes = {name: build(name, *spec) for name, spec in VARIANTS.items()}
    configs = list(itertools.product(
        [int(x) for x in args.guns.split(",")],
        [int(x) for x in args.ships.split(",")]))

    results, checksums = {}, {}
    for _ in range(args.rounds):
        for config in configs:
            for name, exe in exes.items():
                out = subprocess.run(
                    [exe, *map(str, config), str(args.frames),
                     str(args.reps)], check=True, capture_output=True,
                    text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results.setdefault((config, name), []).append(
                    float(fields["ns_per_frame"]))
                checksums.setdefault(config, {}).setdefault(
                    fields["checksum"], set()).add(name)

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")

    names = list(exes)
    print(f"\nus per frame, median of {args.rounds} processes (spread); "
          "ns per gun in brackets; best in bold\n")
    print("| guns | ships | " + " | ".join(names) + " |")
    print("|---|---|" + "---|" * len(names))
    for config in configs:
        medians = {name: statistics.median(results[(config, name)])
                   for name in names}
        best = min(medians.values())
        cells = []
        for name in names:
            ts = results[(config, name)]
            spread = (max(ts) - min(ts)) / medians[name]
            cell = (f"{medians[name] / 1e3:,.1f} ({spread:.0%}) "
                    f"[{medians[name] / config[0]:.2f}]")
            cells.append(f"**{cell}**" if medians[name] == best else cell)
        print(f"| {config[0]:.0e} | {config[1]:.0e} | " + " | ".join(cells)
              + " |")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("variant,guns,ships,round,ns_per_frame\n")
            for ((n, m), name), rows in results.items():
                for i, t in enumerate(rows):
                    f.write(f"{name},{n},{m},{i},{t}\n")


if __name__ == "__main__":
    main()
