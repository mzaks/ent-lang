#!/usr/bin/env python3
"""River network (see river.c): a sum down a tree from its leaves, the
ent-lang program (examples/river.ent, a `for` that cascades `leaves first`
and adds into its parent) against hand-written C.

Every variant is its own binary per size (the world's capacities are
compile-time) and every measurement its own process, round-robin over
configurations and variants. Reports medians over --rounds processes with
their spread and the tree's depth, and checks that all variants agree on
the checksum (a hash of every node's flow bits).

Usage: bench/river/run.py [--rounds 5] [--steps 100] [--warmup 10]
       [--sizes 10000,100000,1000000] [--shapes bushy,deep,shuffled]
       [--variants ...] [--resort]
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
OUT = os.path.join(ROOT, "build", "bench", "river")
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
LOWER = ["--convert-scf-to-cf", "--convert-to-llvm",
         "--reconcile-unrealized-casts"]
OPENMP = ["-fopenmp", f"-I{LLVM}/include", f"-L{LLVM}/lib",
          f"-Wl,-rpath,{LLVM}/lib"]
PARALLEL = ["--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1",
            "--convert-scf-to-openmp", "--canonicalize", "--ent-omp-nowait"]
# name: (VARIANT, program, ent-opt passes before LOWER[, a change to the
# program's text])
VARIANTS = {
    "c-order": (0, None, None),
    "c-pairs": (3, None, None),
    "c-sorted": (1, None, None),
    "ent": (2, "river.ent", ["--ent-lower-to-loops"]),
    # The nodes stored in the tree's order.
    "ent-sorted": (2, "river.ent", ["--ent-lower-to-loops"],
                   lambda text: text.replace(" tree capacity 1024",
                                             " tree sorted capacity 1024")),
    # The sorted tree with a depth's nodes visited in parallel.
    "ent-sorted-par": (2, "river.ent", PARALLEL,
                       lambda text: text.replace(
                           " tree capacity 1024",
                           " tree sorted capacity 1024"), [], True),
    # Nodes of two shapes, half of them in a second archetype: a tree
    # across archetypes, as it is and sorted.
    "ent-two": (2, "river.ent", ["--ent-lower-to-loops"],
                lambda text: text + TWO, ["-DTWO"]),
    "ent-two-sorted": (2, "river.ent", ["--ent-lower-to-loops"],
                       lambda text: text.replace(
                           " tree capacity 1024",
                           " tree sorted capacity 1024") + TWO, ["-DTWO"]),
    # And with a depth's nodes visited in parallel, archetype by archetype.
    "ent-two-sorted-par": (2, "river.ent", PARALLEL,
                           lambda text: text.replace(
                               " tree capacity 1024",
                               " tree sorted capacity 1024") + TWO,
                           ["-DTWO"], True),
    # Nodes that can be destroyed (a system that does it, which the host
    # never runs): ids have generations, and nothing about an edge's ends
    # is taken on trust.
    "ent-mortal": (2, "river.ent", ["--ent-lower-to-loops"],
                   lambda text: text + MORTAL),
    "ent-mortal-sorted": (2, "river.ent", ["--ent-lower-to-loops"],
                          lambda text: text.replace(
                              " tree capacity 1024",
                              " tree sorted capacity 1024") + MORTAL),
    "ent-two-mortal": (2, "river.ent", ["--ent-lower-to-loops"],
                       lambda text: text + TWO + MORTAL, ["-DTWO"]),
    "ent-two-mortal-sorted": (2, "river.ent", ["--ent-lower-to-loops"],
                              lambda text: text.replace(
                                  " tree capacity 1024",
                                  " tree sorted capacity 1024") + TWO
                              + MORTAL, ["-DTWO"]),
}
MORTAL = """
system dry() {
  for e, n: Node where n.rain < 0.0 { e.destroy() }
}
schedule drought() { dry() }
"""
TWO = """
component Still { level: f32 } capacity 1024
archetype Pool { Node, Still } capacity 1024
"""
# A tree sorted across archetypes adds a node's inflows in another order:
# its sums differ in their last bits, and are compared by their total.
REORDERED = {"ent-two-sorted", "ent-two-sorted-par", "ent-two-mortal-sorted"}
DEFAULT = ["c-order", "c-pairs", "c-sorted", "ent", "ent-sorted"]


def build(name, number, program, passes, n, transform=None, defines=(),
          openmp=False):
    exe = os.path.join(OUT, f"{name}-{n}")
    extra = []
    if program:
        directory = exe + ".d"
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(ROOT, "examples", program)) as f:
            text = f.read()
        if transform:
            changed = transform(text)
            assert changed != text, "the program is not what a variant expects"
            text = changed
        text = re.sub(r"capacity 1024\b", f"capacity {n}", text)
        source = os.path.join(directory, program)
        with open(source, "w") as f:
            f.write(text)
        mlir = os.path.join(directory, "river.mlir")
        subprocess.run([ENT_TRANSLATE, "--import-ent", source, "-o", mlir],
                       check=True)
        subprocess.run([ENT_TRANSLATE, "--ent-to-c-header", mlir, "-o",
                        os.path.join(directory, "river_world.h")], check=True)
        lowered = subprocess.run([ENT_OPT, mlir, *passes, *LOWER], check=True,
                                 capture_output=True, text=True).stdout
        ll = os.path.join(directory, "river.ll")
        subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                        "-o", ll], input=lowered, text=True, check=True)
        extra = [ll, "-Wno-override-module", f"-I{directory}"]
    if openmp:
        extra += OPENMP
    subprocess.run([f"{LLVM}/bin/clang", "-O2", *EXTRA_CFLAGS,
                    "-ffp-contract=off", f"-DVARIANT={number}", f"-DN={n}",
                    *defines,
                    os.path.join(HERE, "river.c"), *extra, "-o", exe],
                   check=True)
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--sizes", default="10000,100000,1000000")
    parser.add_argument("--shapes", default="bushy,deep,shuffled")
    parser.add_argument("--variants", default=",".join(DEFAULT),
                        help="comma-separated; also: " + ", ".join(
                            v for v in VARIANTS if v not in DEFAULT))
    parser.add_argument("--resort", action="store_true",
                        help="the host connects an edge before every step: "
                        "the ent variants go through every edge for it")
    parser.add_argument("--divert", action="store_true",
                        help="the program connects an edge before every "
                        "step, which an unsorted tree takes in on the spot")
    parser.add_argument("--move", action="store_true",
                        help="the program puts a node under one that is "
                        "later in the tree's order before every step; the "
                        "tree changes, so give ent variants only")
    parser.add_argument("--csv")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    names = args.variants.split(",")

    sizes = [int(float(s)) for s in args.sizes.split(",")]
    shapes = args.shapes.split(",")
    exes = {(n, name): build(name, *VARIANTS[name][:3], n,
                             *VARIANTS[name][3:])
            for n in sizes for name in names}
    configs = list(itertools.product(sizes, shapes))

    results, depths, checksums, totals = {}, {}, {}, {}
    for _ in range(args.rounds):
        for n, shape in configs:
            for name in names:
                out = subprocess.run(
                    [exes[(n, name)], str(args.steps), str(args.warmup),
                     shape] + ["resort"] * args.resort
                    + ["divert"] * args.divert + ["move"] * args.move,
                    check=True, capture_output=True, text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results.setdefault(((n, shape), name), []).append(
                    float(fields["ns_per_step"]))
                depths[(n, shape)] = int(fields["depth"])
                totals.setdefault((n, shape), {})[name] = float(fields["total"])
                if name not in REORDERED:
                    checksums.setdefault((n, shape), {}).setdefault(
                        fields["checksum"], set()).add(name)

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")
    for config, values in totals.items():
        low, high = min(values.values()), max(values.values())
        if high - low > 1e-4 * abs(high):
            print(f"TOTAL MISMATCH at {config}: {values}")

    print(f"\nus per step, median of {args.rounds} processes (spread); "
          "best in bold\n")
    print("| nodes | shape | depth | " + " | ".join(names) + " |")
    print("|---|---|---|" + "---|" * len(names))
    for config in configs:
        n, shape = config
        medians = {name: statistics.median(results[(config, name)])
                   for name in names}
        best = min(medians.values())
        cells = []
        for name in names:
            ts = results[(config, name)]
            spread = (max(ts) - min(ts)) / medians[name]
            cell = f"{medians[name] / 1e3:,.1f} ({spread:.0%})"
            cells.append(f"**{cell}**" if medians[name] == best else cell)
        print(f"| {n:.0e} | {shape} | {depths[config]:,} | "
              + " | ".join(cells) + " |")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("variant,nodes,shape,round,ns_per_step\n")
            for ((n, shape), name), rows in results.items():
                for i, t in enumerate(rows):
                    f.write(f"{name},{n},{shape},{i},{t}\n")


if __name__ == "__main__":
    main()
