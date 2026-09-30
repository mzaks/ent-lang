#!/usr/bin/env python3
"""Change detection (see reactive.c): the compiled reactive query against
hand-written tracking schemes, over the fraction of units changed per frame
and two amounts of work per redraw.

Every variant is its own binary and every measurement its own process,
round-robin over configurations and variants. Reports medians over
--rounds processes with their spread, the redraws per frame (units a
variant redrew, changed or not), and checks that all variants agree on the
checksum.

Usage: bench/reactive/run.py [--rounds 5] [--frames 50] [--reps 5]
"""

import argparse
import itertools
import os
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
OUT = os.path.join(ROOT, "build", "bench", "reactive")
ECS_OPT = os.path.join(ROOT, "build", "bin", "ecs-opt")
ECS_TRANSLATE = os.path.join(ROOT, "build", "bin", "ecs-translate")
PROGRAM = os.path.join(HERE, "react.mlir")
OPENMP = [f"-I{LLVM}/include", f"-L{LLVM}/lib", f"-Wl,-rpath,{LLVM}/lib"]
LOWER = ["--convert-scf-to-cf", "--convert-to-llvm",
         "--reconcile-unrealized-casts"]
PARALLEL = ["--ecs-lower-to-loops=parallel-entities=1 parallel-min-entities=1",
            "--convert-scf-to-openmp", "--canonicalize", "--ecs-omp-nowait"]
# name: (VARIANT, ecs-opt passes before LOWER or None, OpenMP)
VARIANTS = {
    "c-poll": (0, None, False),
    "c-rowstamp": (1, None, False),
    "c-blockstamp": (2, None, False),
    "c-collector": (3, None, False),
    "c-bevy": (4, None, False),
    "c-unity": (5, None, False),
    "compiled": (6, ["--ecs-lower-to-loops"], False),
    "compiled-par": (6, PARALLEL, True),
}
# The heavy redraw: 32 steps of w = w * 0.999 + 0.001 from w = hp, as
# bar() in reactive.c computes with -DHEAVY.
HEAVY_WORK = """\
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c32 = arith.constant 32 : index
    %decay = arith.constant 0.999 : f32
    %step = arith.constant 0.001 : f32
    %w = scf.for %k = %c0 to %c32 step %c1 iter_args(%acc = %hp) -> f32 {
      %scaled = arith.mulf %acc, %decay : f32
      %next = arith.addf %scaled, %step : f32
      scf.yield %next : f32
    }
"""


def program(work):
    """react.mlir, with the heavy work swapped in if asked for."""
    text = open(PROGRAM).read()
    if work == "light":
        return text
    begin = text.index("    // BEGIN WORK")
    end = text.index("    // END WORK\n") + len("    // END WORK\n")
    return text[:begin] + HEAVY_WORK + text[end:]


def build(name, work, number, passes, openmp):
    directory = os.path.join(OUT, work)
    os.makedirs(directory, exist_ok=True)
    exe = os.path.join(directory, name)
    extra = ["-DHEAVY"] if work == "heavy" else []
    if passes:
        source = os.path.join(directory, "react.mlir")
        with open(source, "w") as f:
            f.write(program(work))
        subprocess.run([ECS_TRANSLATE, "--ecs-to-c-header", source, "-o",
                        os.path.join(directory, "react_world.h")], check=True)
        mlir = subprocess.run([ECS_OPT, source, *passes, *LOWER], check=True,
                              capture_output=True, text=True).stdout
        ll = os.path.join(directory, name + ".ll")
        subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                        "-o", ll], input=mlir, text=True, check=True)
        extra += [ll, "-Wno-override-module", f"-I{directory}"]
    if openmp:
        extra += ["-fopenmp", *OPENMP]
    subprocess.run([f"{LLVM}/bin/clang", "-O2", "-ffp-contract=off",
                    f"-DVARIANT={number}", os.path.join(HERE, "reactive.c"),
                    *extra, "-o", exe], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--frames", type=int, default=50)
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--units", type=int, default=1000000)
    # Thresholds out of 65536: about 0.01%, 0.1%, 1%, 10% and 100%.
    parser.add_argument("--thresholds", default="7,66,655,6554,65536")
    parser.add_argument("--work", default="light,heavy")
    parser.add_argument("--variants", default=",".join(VARIANTS))
    parser.add_argument("--csv")
    args = parser.parse_args()

    works = args.work.split(",")
    names = args.variants.split(",")
    exes = {(work, name): build(name, work, *VARIANTS[name])
            for work in works for name in names}
    configs = list(itertools.product(
        works, [int(t) for t in args.thresholds.split(",")]))

    times, redraws, checksums = {}, {}, {}
    for _ in range(args.rounds):
        for work, threshold in configs:
            for name in names:
                out = subprocess.run(
                    [exes[(work, name)], str(args.units), str(threshold),
                     str(args.frames), str(args.reps)],
                    check=True, capture_output=True, text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                key = ((work, threshold), name)
                times.setdefault(key, []).append(float(fields["ns_per_frame"]))
                redraws[key] = float(fields["redraws_per_frame"])
                checksums.setdefault((work, threshold), {}).setdefault(
                    fields["checksum"], set()).add(name)

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")

    for work in works:
        print(f"\n{work} redraw, {args.units:.0e} units: us per frame, "
              f"median of {args.rounds} processes (spread); redraws per "
              "frame in brackets; best in bold\n")
        print("| changed | " + " | ".join(names) + " |")
        print("|---|" + "---|" * len(names))
        for threshold in [c[1] for c in configs if c[0] == work]:
            config = (work, threshold)
            medians = {name: statistics.median(times[(config, name)])
                       for name in names}
            best = min(medians.values())
            cells = []
            for name in names:
                ts = times[(config, name)]
                spread = (max(ts) - min(ts)) / medians[name]
                cell = (f"{medians[name] / 1e3:,.1f} ({spread:.0%}) "
                        f"[{redraws[(config, name)]:,.0f}]")
                cells.append(f"**{cell}**" if medians[name] == best else cell)
            print(f"| {threshold / 65536:.2%} | " + " | ".join(cells) + " |")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("variant,work,threshold,round,ns_per_frame\n")
            for ((work, threshold), name), rows in times.items():
                for i, t in enumerate(rows):
                    f.write(f"{name},{work},{threshold},{i},{t}\n")


if __name__ == "__main__":
    main()
