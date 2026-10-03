#!/usr/bin/env python3
"""Spiking network (see snn.c): ent-lang relations, pushing spikes along
outgoing synapses (examples/snn.ent) or gathering them along incoming ones
(examples/snn_pull.ent), against hand-written C over compressed rows.

Every variant is its own binary per network size (the world's capacities
are compile-time) and every measurement its own process, round-robin over
configurations and variants. Reports medians over --rounds processes with
their spread and the firing rate, and checks that all variants agree on the
checksum (a hash of every neuron's potential bits).

Usage: bench/snn/run.py [--rounds 5] [--steps 100] [--warmup 50]
       [--sizes 10000x100,100000x100,1000000x20] [--bias 0.15,0.3]
"""

import argparse
import itertools
import os
import re
import statistics
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
# Extra compiler flags for every binary, e.g. BENCH_CFLAGS=-march=native.
EXTRA_CFLAGS = os.environ.get("BENCH_CFLAGS", "").split()
OUT = os.path.join(ROOT, "build", "bench", "snn")
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
OPENMP = ["-fopenmp", f"-I{LLVM}/include", f"-L{LLVM}/lib",
          f"-Wl,-rpath,{LLVM}/lib"]
LOWER = ["--convert-scf-to-cf", "--convert-to-llvm",
         "--reconcile-unrealized-casts"]
PARALLEL = ["--ent-lower-to-loops=parallel-entities=1 parallel-min-entities=1",
            "--convert-scf-to-openmp", "--canonicalize", "--ent-omp-nowait"]
# name: (VARIANT, program, ent-opt passes before LOWER, OpenMP)
VARIANTS = {
    "c-push": (0, None, None, False),
    "c-pull": (1, None, None, False),
    "c-pull-par": (2, None, None, True),
    "ent-push": (3, "snn.ent", ["--ent-lower-to-loops"], False),
    "ent-push-par": (3, "snn.ent", PARALLEL, True),
    "ent-pull": (3, "snn_pull.ent", ["--ent-lower-to-loops"], False),
    "ent-pull-par": (3, "snn_pull.ent", PARALLEL, True),
    # Diagnostic: c-pull plus one more thing the generated pull does each.
    "c-pull-index": (4, None, None, False),
    "c-pull-index-store": (5, None, None, False),
    "c-pull-index-store-locate": (6, None, None, False),
    "c-pull-locate": (7, None, None, False),
    "c-push-buffer": (8, None, None, False),
    "c-push-fixed": (10, None, None, False),
    # Only the integration: the example's schedule without propagate.
    "c-integrate": (9, None, None, False),
    "ent-integrate": (3, "snn.ent", ["--ent-lower-to-loops"], False,
                      lambda text: text.replace("  propagate()\n", "")),
    # ent-push with applies always buffered until the query ends.
    "ent-push-buffered": (3, "snn.ent",
                          ["--ent-lower-to-loops=direct-applies=0"], False),
}
DEFAULT = ["c-push", "c-pull", "c-pull-par", "ent-push", "ent-push-par",
           "ent-pull", "ent-pull-par"]


def build(name, number, program, passes, openmp, n, k, transform=None):
    exe = os.path.join(OUT, f"{name}-{n}x{k}")
    extra = []
    if program:
        directory = exe + ".d"
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(ROOT, "examples", program)) as f:
            text = f.read()
        text = re.sub(r"capacity 1024\b", f"capacity {n}", text)
        text = re.sub(r"capacity 65536\b", f"capacity {n * k}", text)
        if transform:
            text = transform(text)
        source = os.path.join(directory, program)
        with open(source, "w") as f:
            f.write(text)
        mlir = os.path.join(directory, "snn.mlir")
        subprocess.run([ENT_TRANSLATE, "--import-ent", source, "-o", mlir],
                       check=True)
        subprocess.run([ENT_TRANSLATE, "--ent-to-c-header", mlir, "-o",
                        os.path.join(directory, "snn_world.h")], check=True)
        lowered = subprocess.run([ENT_OPT, mlir, *passes, *LOWER], check=True,
                                 capture_output=True, text=True).stdout
        ll = os.path.join(directory, "snn.ll")
        subprocess.run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir",
                        "-o", ll], input=lowered, text=True, check=True)
        extra = [ll, "-Wno-override-module", f"-I{directory}"]
    if openmp:
        extra += OPENMP
    subprocess.run([f"{LLVM}/bin/clang", "-O2", *EXTRA_CFLAGS,
                    "-ffp-contract=off",
                    f"-DVARIANT={number}", f"-DN={n}", f"-DK={k}",
                    os.path.join(HERE, "snn.c"), *extra, "-o", exe],
                   check=True)
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--sizes", default="10000x100,100000x100,1000000x20")
    parser.add_argument("--bias", default="0.15,0.3")
    parser.add_argument("--variants", default=",".join(DEFAULT),
                        help="comma-separated; also: " + ", ".join(
                            v for v in VARIANTS if v not in DEFAULT))
    parser.add_argument("--csv")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    names = args.variants.split(",")

    sizes = [tuple(int(x) for x in s.split("x")) for s in args.sizes.split(",")]
    biases = args.bias.split(",")
    exes = {(size, name): build(name, *VARIANTS[name][:4], *size,
                                *VARIANTS[name][4:])
            for size in sizes for name in names}
    configs = list(itertools.product(sizes, biases))

    results, rates, checksums = {}, {}, {}
    for _ in range(args.rounds):
        for size, bias in configs:
            for name in names:
                out = subprocess.run(
                    [exes[(size, name)], str(args.steps), str(args.warmup),
                     bias], check=True, capture_output=True, text=True).stdout
                fields = dict(kv.split("=") for kv in out.split())
                results.setdefault(((size, bias), name), []).append(
                    float(fields["ns_per_step"]))
                rates[(size, bias)] = float(fields["spikes_per_step"])
                checksums.setdefault((size, bias), {}).setdefault(
                    fields["checksum"], set()).add(name)

    for config, values in checksums.items():
        if len(values) != 1:
            print(f"CHECKSUM MISMATCH at {config}: {values}")

    print(f"\nus per step, median of {args.rounds} processes (spread); "
          "best in bold\n")
    print("| neurons x synapses | bias | firing | " + " | ".join(names) + " |")
    print("|---|---|---|" + "---|" * len(names))
    for (size, bias) in configs:
        config = (size, bias)
        medians = {name: statistics.median(results[(config, name)])
                   for name in names}
        best = min(medians.values())
        cells = []
        for name in names:
            ts = results[(config, name)]
            spread = (max(ts) - min(ts)) / medians[name]
            cell = f"{medians[name] / 1e3:,.1f} ({spread:.0%})"
            cells.append(f"**{cell}**" if medians[name] == best else cell)
        firing = rates[config] / size[0]
        print(f"| {size[0]:.0e} x {size[1]} | {bias} | {firing:.1%} | "
              + " | ".join(cells) + " |")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("variant,neurons,synapses,bias,round,ns_per_step\n")
            for (((n, k), bias), name), rows in results.items():
                for i, t in enumerate(rows):
                    f.write(f"{name},{n},{k},{bias},{i},{t}\n")


if __name__ == "__main__":
    main()
