#!/usr/bin/env python3
"""Scene graph (see scene.c): every node placed at its parent's place plus
its own offset, by a `for` that cascades over all of them every step
(`full`) against one that is reactive too (`reactive`: the nodes whose
offset changed and what is below them), with the tree as it is and
`sorted`. The host moves --moves nodes a step.

Every variant is its own binary per size and every measurement its own
process, round-robin. Reports medians over --rounds processes with their
spread and how many nodes a step's moves reach, and checks that all
variants agree on the checksum.

Usage: bench/scene/run.py [--rounds 5] [--steps 100] [--warmup 10]
       [--sizes 100000,1000000] [--shapes bushy,deep]
       [--moves 0,1,10,100,1000]
"""

import argparse
import itertools
import os
import platform
import statistics
import subprocess
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
# Flags for every binary: the machine's own instruction set, unless
# BENCH_CFLAGS says otherwise (BENCH_CFLAGS= for the baseline one).
NATIVE = ("-mcpu=native" if platform.machine() in ("arm64", "aarch64")
          else "-march=native")
EXTRA_CFLAGS = os.environ.get("BENCH_CFLAGS", NATIVE).split()
OUT = os.path.join(ROOT, "build", "bench", "scene")
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
LOWER = ["--convert-scf-to-cf", "--convert-to-llvm",
         "--reconcile-unrealized-casts"]
OPENMP = ["-fopenmp", f"-I{LLVM}/include", f"-L{LLVM}/lib",
          f"-Wl,-rpath,{LLVM}/lib"]
PARALLEL = ["--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1",
            "--convert-scf-to-openmp", "--canonicalize", "--ent-omp-nowait"]
# Half of the nodes in a second archetype (scene.c, -DTWO).
TWO = ("archetype Picks {", "component Mass { m: i32 } capacity 1024\n"
       "archetype Heavy { Local, World, Mass } capacity 1024\n"
       "archetype Picks {")
PLAIN = "top down Under {"
REACTIVE = "top down Under\n      on changed l, changed above {"
SORTED = (" tree capacity 1024", " tree sorted capacity 1024")
# name: the changes to the program's text
VARIANTS = {
    "full": [],
    "reactive": [(PLAIN, REACTIVE)],
    "full-sorted": [SORTED],
    "reactive-sorted": [(PLAIN, REACTIVE), SORTED],
    # The sorted tree across two archetypes; `-par` with parallel loops.
    "full-two-sorted": [SORTED, TWO],
    "full-two-sorted-par": [SORTED, TWO],
    "full-sorted-par": [SORTED],
    "reactive-two-sorted": [(PLAIN, REACTIVE), SORTED, TWO],
    # The tree's children in the order of their offsets, which every move
    # writes: the tree's order is made again every step that moves one.
    "full-ordered": [(" tree capacity 1024",
                      " tree ordered by Local.x capacity 1024")],
}
DEFAULT = ["full", "reactive", "full-sorted", "reactive-sorted"]


def build(name, n):
    exe = os.path.join(OUT, f"{name}-{n}")
    directory = exe + ".d"
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(HERE, "scene.ent")) as f:
        text = f.read()
    for old, new in VARIANTS[name]:
        assert text.count(old) == 1, "the program is not what a variant expects"
        text = text.replace(old, new)
    # (The host names at most 1024 nodes a step: Picks keeps its capacity.)
    text = re.sub(r"capacity 1024\b(?!\n\nsystem)", f"capacity {n}", text)
    source = os.path.join(directory, "scene.ent")
    with open(source, "w") as f:
        f.write(text)
    mlir = os.path.join(directory, "scene.mlir")
    subprocess.run([ENT_TRANSLATE, "--import-ent", source, "-o", mlir],
                   check=True)
    subprocess.run([ENT_TRANSLATE, "--ent-to-c-header", mlir, "-o",
                    os.path.join(directory, "scene_world.h")], check=True)
    parallel = name.endswith("-par")
    passes = PARALLEL if parallel else ["--ent-lower-to-loops"]
    lowered = subprocess.run([ENT_OPT, mlir, *passes, *LOWER],
                             check=True, capture_output=True, text=True).stdout
    ll = os.path.join(directory, "scene.ll")
    subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                    "-o", ll], input=lowered, text=True, check=True)
    subprocess.run([f"{LLVM}/bin/clang", "-O2", *EXTRA_CFLAGS, f"-DN={n}",
                    *(["-DTWO"] if "-two-" in name else []),
                    os.path.join(HERE, "scene.c"), ll,
                    *(OPENMP if parallel else []),
                    "-Wno-override-module", f"-I{directory}", "-o", exe],
                   check=True)
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--sizes", default="100000,1000000")
    parser.add_argument("--shapes", default="bushy,deep")
    parser.add_argument("--moves", default="0,1,10,100,1000")
    parser.add_argument("--variants", default=",".join(DEFAULT),
                        help="comma-separated; also: " + ", ".join(
                            v for v in VARIANTS if v not in DEFAULT))
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    names = args.variants.split(",")
    sizes = [int(float(s)) for s in args.sizes.split(",")]
    exes = {(n, name): build(name, n) for n in sizes for name in names}
    configs = list(itertools.product(sizes, args.shapes.split(","),
                                     [int(m) for m in args.moves.split(",")]))

    results, reached, checksums = {}, {}, {}
    for _ in range(args.rounds):
        for config in configs:
            n, shape, moves = config
            for name in names:
                out = subprocess.run(
                    [exes[(n, name)], str(args.steps), str(args.warmup),
                     shape, str(moves)],
                    check=True, capture_output=True, text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results.setdefault((config, name), []).append(
                    float(fields["ns_per_step"]))
                reached[config] = float(fields["reached"])
                checksums.setdefault(config, {}).setdefault(
                    fields["checksum"], set()).add(name)

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")

    print(f"\nus per step, median of {args.rounds} processes (spread); "
          "best in bold\n")
    print("| nodes | shape | moves | reached | " + " | ".join(names) + " |")
    print("|---|---|---|---|" + "---|" * len(names))
    for config in configs:
        n, shape, moves = config
        medians = {name: statistics.median(results[(config, name)])
                   for name in names}
        best = min(medians.values())
        cells = []
        for name in names:
            ts = results[(config, name)]
            spread = (max(ts) - min(ts)) / medians[name]
            cell = f"{medians[name] / 1e3:,.1f} ({spread:.0%})"
            cells.append(f"**{cell}**" if medians[name] == best else cell)
        print(f"| {n:.0e} | {shape} | {moves} | {reached[config]:,.0f} | "
              + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
