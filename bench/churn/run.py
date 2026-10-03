#!/usr/bin/env python3
"""Churn experiment (see churn.c): four storage schemes for an optional
component, swept over entity count, density and churn rate.

Every variant is its own binary and every measurement its own process,
round-robin over configurations and variants. Reports medians over
--rounds processes, with the churn / systems split, and checks that all
variants agree on the checksum.

Usage: bench/churn/run.py [--rounds 5] [--frames 64] [--reps 5]
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
OUT = os.path.join(ROOT, "build", "bench", "churn")
VARIANTS = {"archetypes": 0, "wide-select": 1, "wide-branch": 2,
            "sparse-set": 3, "compiled": 4, "compiled-fused": 4}
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
PROGRAM = os.path.join(HERE, "status.mlir")
LOWERINGS = {"compiled": "--ent-lower-to-loops",
             "compiled-fused": "--ent-lower-to-loops=fuse-systems=1"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--frames", type=int, default=64)
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--sizes", default="100000,1000000")
    parser.add_argument("--densities", default="0.01,0.1,0.5,0.9")
    parser.add_argument("--churns", default="0,0.001,0.01,0.1")
    parser.add_argument("--csv")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)

    subprocess.run([ENT_TRANSLATE, "--ent-to-c-header", PROGRAM, "-o",
                    os.path.join(OUT, "status_world.h")], check=True)
    exes = {}
    for name, number in VARIANTS.items():
        exes[name] = os.path.join(OUT, name)
        extra = []
        if name in LOWERINGS:
            mlir = subprocess.run(
                [ENT_OPT, PROGRAM, LOWERINGS[name], "--symbol-dce",
                 "--convert-scf-to-cf", "--convert-to-llvm",
                 "--reconcile-unrealized-casts"], check=True,
                capture_output=True, text=True).stdout
            ll = os.path.join(OUT, name + ".ll")
            subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                            "-o", ll], input=mlir, text=True, check=True)
            extra = [ll, "-Wno-override-module"]
        subprocess.run([f"{LLVM}/bin/clang", "-O2", *EXTRA_CFLAGS,
                        "-ffp-contract=off",
                        f"-DVARIANT={number}", f"-I{OUT}",
                        os.path.join(HERE, "churn.c"), *extra,
                        "-o", exes[name]], check=True)

    configs = []
    for n, d, c in itertools.product(
            [int(x) for x in args.sizes.split(",")],
            [float(x) for x in args.densities.split(",")],
            [float(x) for x in args.churns.split(",")]):
        # Each frame, churn * n / 2 entities lose Status and as many gain it.
        if c / 2 <= min(d, 1 - d):
            configs.append((n, d, c))

    results = {}
    checksums = {}
    for _ in range(args.rounds):
        for config in configs:
            n, d, c = config
            for name, exe in exes.items():
                out = subprocess.run(
                    [exe, str(n), str(d), str(c), str(args.frames),
                     str(args.reps)], check=True, capture_output=True,
                    text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results.setdefault((config, name), []).append(fields)
                checksums.setdefault(config, set()).add(fields["checksum"])

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")

    def median(config, name, key):
        return statistics.median(float(f[key])
                                 for f in results[(config, name)])

    def spread(config, name):
        ts = [float(f["ns_per_frame"]) for f in results[(config, name)]]
        return (max(ts) - min(ts)) / statistics.median(ts)

    names = list(exes)
    for n in sorted({cfg[0] for cfg in configs}):
        print(f"\nn={n:.0e}: us per frame, median of {args.rounds} "
              f"processes (spread); best variant in bold\n")
        print("| density | churn/frame | " + " | ".join(names) + " |")
        print("|---|---|" + "---|" * len(names))
        for cfg in [c for c in configs if c[0] == n]:
            totals = {name: median(cfg, name, "ns_per_frame")
                      for name in names}
            best = min(totals.values())
            cells = []
            for name in names:
                cell = f"{totals[name] / 1e3:,.1f} ({spread(cfg, name):.0%})"
                cells.append(f"**{cell}**" if totals[name] == best else cell)
            print(f"| {cfg[1]:.0%} | {cfg[2]:.1%} | " + " | ".join(cells)
                  + " |")
        print(f"\nn={n:.0e}: churn / systems us per frame, and MB "
              f"allocated\n")
        print("| density | churn/frame | " + " | ".join(names) + " |")
        print("|---|---|" + "---|" * len(names))
        for cfg in [c for c in configs if c[0] == n]:
            cells = [f"{median(cfg, name, 'churn_ns') / 1e3:,.1f} / "
                     f"{median(cfg, name, 'systems_ns') / 1e3:,.1f}"
                     for name in names]
            print(f"| {cfg[1]:.0%} | {cfg[2]:.1%} | " + " | ".join(cells)
                  + " |")
        cfg = [c for c in configs if c[0] == n][0]
        print("\nMB allocated at density " +
              ", ".join(f"{c[1]:.0%}" for c in configs
                        if c[0] == n and c[2] == 0) + ": " +
              "; ".join(f"{name} " + ", ".join(
                  f"{median(c, name, 'bytes') / 1e6:.1f}"
                  for c in configs if c[0] == n and c[2] == 0)
                  for name in names))

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("variant,n,density,churn,round,ns_per_frame,churn_ns,"
                    "systems_ns,bytes\n")
            for ((n, d, c), name), rows in results.items():
                for i, r in enumerate(rows):
                    f.write(f"{name},{n},{d},{c},{i},{r['ns_per_frame']},"
                            f"{r['churn_ns']},{r['systems_ns']},"
                            f"{r['bytes']}\n")


if __name__ == "__main__":
    main()
