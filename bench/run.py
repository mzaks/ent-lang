#!/usr/bin/env python3
"""Build every variant of the integrate benchmark and time it.

Each variant is its own binary and every measurement its own process.
Processes run round-robin over sizes and variants, so slow drift (thermal,
background load) spreads over all variants instead of biasing one.

Usage: bench/run.py [--rounds 5] [--threads 12] [--blocktime 200]
                    [--sizes 1000,...] [--variants a,b] [--csv out]

--blocktime sets KMP_BLOCKTIME for the OpenMP runtime. Homebrew's libomp
defaults to 0 ms with a passive wait policy, so workers sleep after every
parallel region and each fork pays a wake-up (about 35 us with 12 threads
on an M4 Max); 200 is the value the OpenMP documentation gives as default.
"""

import argparse
import os
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLVM = os.environ.get("LLVM_PREFIX", "/opt/homebrew/opt/llvm")
ECS_OPT = os.path.join(ROOT, "build", "bin", "ecs-opt")
OUT = os.path.join(ROOT, "build", "bench")
EXAMPLE = os.path.join(ROOT, "examples", "integrate.mlir")
HOST = os.path.join(ROOT, "bench", "bench_main.c")
REFERENCE = os.path.join(ROOT, "bench", "reference.c")
TO_LLVM = ["--symbol-dce", "--convert-scf-to-openmp", "--convert-scf-to-cf",
           "--convert-to-llvm", "--reconcile-unrealized-casts"]
OPENMP = [f"-L{LLVM}/lib", "-lomp", f"-Wl,-rpath,{LLVM}/lib"]

# name -> ecs-opt passes before lowering to LLVM, or None for C references.
VARIANTS = {
    "loops": ["--ecs-lower-to-loops"],
    "stages-omp": ["--ecs-schedule", "--ecs-lower-to-loops=parallel-stages=1"],
    "entities-omp": ["--ecs-lower-to-loops=parallel-entities=1"],
    "fused": ["--ecs-lower-to-loops=fuse-systems=1"],
    "fused-entities-omp": [
        "--ecs-lower-to-loops=fuse-systems=1 parallel-entities=1"],
    "c-fused": None,
    "c-fused-restrict": None,
}


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def build(name, passes):
    exe = os.path.join(OUT, name)
    cflags = ["-O2", "-Wno-override-module"]
    if passes is None:
        defines = ["-DRESTRICT=restrict"] if name.endswith("restrict") else []
        run([f"{LLVM}/bin/clang", *cflags, "-ffp-contract=off", *defines,
             REFERENCE, HOST, "-o", exe])
        return exe
    mlir = run([ECS_OPT, EXAMPLE, *passes, *TO_LLVM], capture_output=True,
               text=True).stdout
    ll = os.path.join(OUT, name + ".ll")
    run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir", "-o", ll],
        input=mlir, text=True)
    run([f"{LLVM}/bin/clang", *cflags, ll, HOST, *OPENMP, "-o", exe])
    return exe


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--reps", type=int, default=3,
                        help="timed repetitions inside each process")
    parser.add_argument("--threads", type=int, default=12)
    parser.add_argument("--target-ms", type=int, default=200,
                        help="duration of one timed repetition")
    parser.add_argument("--blocktime",
                        help="KMP_BLOCKTIME for the OpenMP runtime")
    parser.add_argument("--sizes", default="1000,10000,100000,1000000,10000000")
    parser.add_argument("--variants", default=",".join(VARIANTS))
    parser.add_argument("--csv")
    args = parser.parse_args()

    os.makedirs(OUT, exist_ok=True)
    names = args.variants.split(",")
    exes = {name: build(name, VARIANTS[name]) for name in names}
    sizes = [int(s) for s in args.sizes.split(",")]
    env = dict(os.environ, OMP_NUM_THREADS=str(args.threads))
    if args.blocktime is not None:
        env["KMP_BLOCKTIME"] = args.blocktime

    times = {(n, v): [] for n in sizes for v in names}
    checksums = {}
    for round_index in range(args.rounds):
        for n in sizes:
            for name in names:
                out = run([exes[name], str(n), str(args.reps),
                           str(args.target_ms)],
                          capture_output=True, text=True, env=env).stdout
                fields = dict(kv.split("=") for kv in out.split())
                times[(n, name)].append(float(fields["ns_per_frame"]))
                checksums.setdefault(n, {})[name] = fields["checksum"]
        print(f"round {round_index + 1}/{args.rounds} done", file=sys.stderr)

    for n in sizes:
        distinct = set(checksums[n].values())
        if len(distinct) != 1:
            print(f"CHECKSUM MISMATCH at n={n}: {checksums[n]}")

    print(f"\nns per frame, median over {args.rounds} processes "
          f"(best of {args.reps} each); OMP_NUM_THREADS={args.threads}, "
          f"KMP_BLOCKTIME={args.blocktime or 'runtime default'}; "
          f"spread = (max-min)/median\n")
    header = "| variant | " + " | ".join(f"n={n:.0e}" for n in sizes) + " |"
    print(header)
    print("|" + "---|" * (len(sizes) + 1))
    for name in names:
        cells = []
        for n in sizes:
            ts = times[(n, name)]
            med = statistics.median(ts)
            spread = (max(ts) - min(ts)) / med
            cells.append(f"{med:,.0f} ({spread:.0%})")
        print(f"| {name} | " + " | ".join(cells) + " |")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("variant,n,round,ns_per_frame\n")
            for (n, name), ts in times.items():
                for i, t in enumerate(ts):
                    f.write(f"{name},{n},{i},{t}\n")


if __name__ == "__main__":
    main()
