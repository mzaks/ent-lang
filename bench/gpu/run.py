#!/usr/bin/env python3
"""The integrate benchmark's frame on the GPU, against the CPU.

  python3 bench/gpu/run.py [--rounds 3] [--threads 8] [--sizes 10000,...]

Variants, every one of which must give the same checksum:

  fused      examples/integrate.mlir, fused, on one core
  fused-omp  the same with parallel entity loops (OpenMP, --threads)
  g0         G0's prototype: integrate.mlir's fused entity-parallel loops
             made kernels by upstream passes alone (every parallel loop
             mapped), on gpu_runtime.c here, which creates a stream and
             waits for it around every launch; the world in managed memory
             (bench_main.c built with posix_memalign renamed)
  gpu        G1 (docs/gpu.md): examples/integrate.ent, whose systems are
             `gpu`, built as `ent build --gpu` builds a program: the world
             copied to the GPU once and kept there (the CPU touches none of
             what the frame's kernels do), one stream, the CPU waiting when
             the frame ends; devices/gpu.c, the host built with ENT_GPU
"""

import argparse
import collections
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LLVM = os.environ.get(
    "LLVM_PREFIX", os.path.join(ROOT, "build/toolchain/.pixi/envs/default"))
CUDA = os.environ.get("CUDA_PATH", "/opt/cuda")
CC = os.environ.get("ENT_GPU_CC", "clang")
CFLAGS = ["-O2", "-march=native", "-Wno-override-module"]
ENT_OPT = os.path.join(ROOT, "build", "bin", "ent-opt")
ENT_TRANSLATE = os.path.join(ROOT, "build", "bin", "ent-translate")
OUT = os.path.join(ROOT, "build", "bench", "gpu")
EXAMPLE = os.path.join(ROOT, "examples", "integrate.mlir")
SOURCE = os.path.join(ROOT, "examples", "integrate.ent")
HOST = os.path.join(ROOT, "bench", "bench_main.c")
RUNTIME = os.path.join(HERE, "gpu_runtime.c")
DEVICE = os.path.join(ROOT, "devices", "gpu.c")

TO_LLVM = ["--symbol-dce", "--convert-scf-to-openmp", "--canonicalize",
           "--ent-omp-nowait", "--convert-scf-to-cf", "--convert-to-llvm",
           "--reconcile-unrealized-casts"]
# sm_89 is the RTX 40 series; it needs PTX 7.8 or later, more than the
# NVPTX backend's default.
TO_GPU = ["--scf-parallel-loop-tiling=parallel-loop-tile-sizes=256 "
          "no-min-max-bounds=true",
          "--gpu-map-parallel-loops", "--convert-parallel-loops-to-gpu",
          "--gpu-kernel-outlining", "--convert-scf-to-cf"]
OPENMP = [f"-L{LLVM}/lib", "-lomp", f"-Wl,-rpath,{LLVM}/lib"]
VARIANTS = ["fused", "fused-omp", "g0", "gpu"]


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def lower(passes, out):
    mlir = run([ENT_OPT, EXAMPLE, *passes], capture_output=True,
               text=True).stdout
    run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir", "-o", out],
        input=mlir, text=True)


def build(chip):
    os.makedirs(OUT, exist_ok=True)
    run([ENT_TRANSLATE, "--ent-to-c-header", EXAMPLE, "-o",
         os.path.join(OUT, "integrate_world.h")])
    cflags = CFLAGS + [f"-I{OUT}"]
    for name, passes in (("fused", "fuse-systems=1"),
                         ("fused-omp", "fuse-systems=1 parallel-entities=1")):
        ll = os.path.join(OUT, name + ".ll")
        lower([f"--ent-lower-to-loops={passes}", *TO_LLVM], ll)
        run([CC, *cflags, ll, HOST, *OPENMP, "-o", os.path.join(OUT, name)])

    # G0: every entity loop on the GPU, however few entities it has.
    loops = run([ENT_OPT, EXAMPLE, "--ent-lower-to-loops=fuse-systems=1 "
                 "parallel-entities=1 parallel-min-entities=1",
                 "--symbol-dce"], capture_output=True, text=True).stdout
    mlir = run([f"{LLVM}/bin/mlir-opt", *TO_GPU,
                f"--gpu-lower-to-nvvm-pipeline=cubin-chip={chip} "
                "cubin-features=+ptx80 cubin-format=isa"],
               input=loops, capture_output=True, text=True).stdout
    ll = os.path.join(OUT, "gpu.ll")
    run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir", "-o", ll],
        input=mlir, text=True)
    run([CC, *cflags, f"-I{CUDA}/include",
         "-Dposix_memalign=ent_gpu_memalign", ll, HOST, RUNTIME, "-lcuda",
         "-o", os.path.join(OUT, "g0")])

    # G1: the `gpu` systems of integrate.ent, by the passes of
    # `ent build --gpu` (tools/ent, gpu_passes), with its own header.
    g1 = os.path.join(OUT, "g1")
    os.makedirs(g1, exist_ok=True)
    module = os.path.join(g1, "integrate.mlir")
    run([ENT_TRANSLATE, "--import-ent", SOURCE, "-o", module])
    run([ENT_TRANSLATE, "--ent-to-c-header", module, "-o",
         os.path.join(g1, "integrate_world.h")])
    ll = os.path.join(OUT, "g1.ll")
    lowered = run([ENT_OPT, module,
                   "--ent-lower-to-loops=fuse-systems=1 gpu=1",
                   "--symbol-dce", "--cse", "--ent-lower-views",
                   "--convert-parallel-loops-to-gpu", "--gpu-kernel-outlining",
                   "--lower-affine", "--convert-scf-to-cf",
                   f"--nvvm-attach-target=chip={chip} features=+ptx80",
                   "--convert-gpu-to-nvvm", "--reconcile-unrealized-casts",
                   "--convert-to-llvm", "--gpu-to-llvm",
                   "--reconcile-unrealized-casts",
                   "--gpu-module-to-binary=format=isa"],
                  capture_output=True, text=True).stdout
    run([f"{LLVM}/bin/mlir-translate", "--mlir-to-llvmir", "-o", ll],
        input=lowered, text=True)
    run([CC, *CFLAGS, f"-I{g1}", f"-I{CUDA}/include", "-DENT_GPU", ll, HOST,
         DEVICE, "-lcuda", "-o", os.path.join(OUT, "gpu")])


def measure(variant, n, threads, target_ms):
    exe = os.path.join(OUT, "gpu" if variant.startswith("gpu") else variant)
    env = dict(os.environ, OMP_NUM_THREADS=str(threads))
    out = run([exe, str(n), "3", str(target_ms)], env=env,
              capture_output=True, text=True).stdout
    m = re.search(r"ns_per_frame=([\d.]+) checksum=(\S+)", out)
    return float(m.group(1)), m.group(2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--target-ms", type=int, default=200)
    parser.add_argument("--chip", default="sm_89")
    parser.add_argument("--sizes", default="10000,100000,1000000,10000000")
    args = parser.parse_args()
    sizes = [int(s) for s in args.sizes.split(",")]

    build(args.chip)
    times = collections.defaultdict(list)
    sums = collections.defaultdict(set)
    for _ in range(args.rounds):
        for n in sizes:
            for v in VARIANTS:
                t, checksum = measure(v, n, args.threads, args.target_ms)
                times[(v, n)].append(t)
                sums[n].add(checksum)

    print("| entities | " + " | ".join(VARIANTS) + " |")
    print("|---" * (len(VARIANTS) + 1) + "|")
    for n in sizes:
        cells = [f"{statistics.median(times[(v, n)]) / 1000:,.1f} us"
                 for v in VARIANTS]
        print(f"| {n:,} | " + " | ".join(cells) + " |")
    bad = [n for n in sizes if len(sums[n]) != 1]
    if bad:
        print("checksums differ at", bad, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
