#!/usr/bin/env python3
"""Edge loops over a tree (see edges.c): edges.ent as it is, its relation
a tree, and with `tree` taken out, a table of edges, against C by hand;
gathering along the edges into each node (pull) and sending along each
node's edge out (push).

Every variant is its own binary per size and every measurement its own
process, round-robin. Reports medians over --rounds processes with their
spread and checks that all variants agree on the checksum.

Usage: bench/river/edges.py [--rounds 5] [--steps 100] [--warmup 10]
       [--sizes 10000,100000,1000000] [--shapes bushy,deep,shuffled]
"""

import argparse
import itertools
import os
import platform
import re
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
# Flags for every binary: the machine's own instruction set, unless
# BENCH_CFLAGS says otherwise (BENCH_CFLAGS= for the baseline one).
NATIVE = ("-mcpu=native" if platform.machine() in ("arm64", "aarch64")
          else "-march=native")
EXTRA_CFLAGS = os.environ.get("BENCH_CFLAGS", NATIVE).split()
OUT = os.path.join(ROOT, "build", "bench", "river-edges")
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
LOWER = ["--convert-scf-to-cf", "--convert-to-llvm",
         "--reconcile-unrealized-casts"]
# name: (VARIANT, whether it is the ent-lang program, a change to its text)
VARIANTS = {
    "c": (0, False, None),
    "ent-tree": (2, True, None),
    "ent-table": (2, True, lambda text: text.replace(
        " tree capacity 1024", " capacity 1024")),
}


def build(name, number, program, transform, n):
    exe = os.path.join(OUT, f"{name}-{n}")
    extra = []
    if program:
        directory = exe + ".d"
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(HERE, "edges.ent")) as f:
            text = f.read()
        if transform:
            changed = transform(text)
            assert changed != text, "the program is not what a variant expects"
            text = changed
        text = re.sub(r"capacity 1024\b", f"capacity {n}", text)
        source = os.path.join(directory, "edges.ent")
        with open(source, "w") as f:
            f.write(text)
        mlir = os.path.join(directory, "edges.mlir")
        subprocess.run([ENT_TRANSLATE, "--import-ent", source, "-o", mlir],
                       check=True)
        subprocess.run([ENT_TRANSLATE, "--ent-to-c-header", mlir, "-o",
                        os.path.join(directory, "edges_world.h")], check=True)
        lowered = subprocess.run([ENT_OPT, mlir, "--ent-lower-to-loops",
                                  *LOWER], check=True,
                                 capture_output=True, text=True).stdout
        ll = os.path.join(directory, "edges.ll")
        subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                        "-o", ll], input=lowered, text=True, check=True)
        extra = [ll, "-Wno-override-module", f"-I{directory}"]
    subprocess.run([f"{LLVM}/bin/clang", "-O2", *EXTRA_CFLAGS,
                    "-ffp-contract=off", f"-DVARIANT={number}", f"-DN={n}",
                    os.path.join(HERE, "edges.c"), *extra, "-o", exe],
                   check=True)
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--sizes", default="10000,100000,1000000")
    parser.add_argument("--shapes", default="bushy,deep,shuffled")
    parser.add_argument("--variants", default=",".join(VARIANTS))
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    names = args.variants.split(",")
    sizes = [int(float(s)) for s in args.sizes.split(",")]
    shapes = args.shapes.split(",")
    exes = {(n, name): build(name, *VARIANTS[name], n)
            for n in sizes for name in names}
    configs = list(itertools.product(sizes, shapes, ["pull", "push"]))

    results, checksums = {}, {}
    for _ in range(args.rounds):
        for n, shape, way in configs:
            for name in names:
                out = subprocess.run(
                    [exes[(n, name)], str(args.steps), str(args.warmup),
                     shape, way], check=True, capture_output=True,
                    text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results.setdefault(((n, shape, way), name), []).append(
                    float(fields["ns_per_step"]))
                checksums.setdefault((n, shape, way), {}).setdefault(
                    fields["checksum"], set()).add(name)

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")

    print(f"\nus per step, median of {args.rounds} processes (spread); "
          "best in bold\n")
    print("| nodes | shape | way | " + " | ".join(names) + " |")
    print("|---|---|---|" + "---|" * len(names))
    for config in configs:
        n, shape, way = config
        medians = {name: statistics.median(results[(config, name)])
                   for name in names}
        best = min(medians.values())
        cells = []
        for name in names:
            ts = results[(config, name)]
            spread = (max(ts) - min(ts)) / medians[name]
            cell = f"{medians[name] / 1e3:,.1f} ({spread:.0%})"
            cells.append(f"**{cell}**" if medians[name] == best else cell)
        print(f"| {n:.0e} | {shape} | {way} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
