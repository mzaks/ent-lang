#!/usr/bin/env python3
"""G0: the integrate benchmark's frame on the GPU, against the CPU.

  python3 bench/gpu/run.py [--rounds 3] [--threads 8] [--sizes 10000,...]

A prototype with no change to the language or the compiler: ent-opt lowers
examples/integrate.mlir with fused, entity-parallel loops, and upstream
MLIR passes make kernels of the parallel loops (blocks of 256 threads),
lower them to NVVM and embed their PTX in the program, which the CUDA
driver compiles when the program starts. gpu_runtime.c has the mgpu*
functions the lowering calls, over the driver API, and the world's arena:
bench_main.c is built with posix_memalign renamed to ent_gpu_memalign.

Variants: fused (one core), fused-omp (OpenMP, --threads), gpu-managed
(the arena in unified memory: it moves to the GPU and stays there while
only the GPU touches it) and gpu-host (the arena in pinned host memory,
which the GPU reads and writes over PCIe each frame). Every variant must
give the same checksum.

Needs an NVIDIA GPU, the CUDA toolkit (CUDA_PATH, else /opt/cuda) and LLVM
with MLIR (LLVM_PREFIX, else build/toolchain's environment). Binaries are
linked by the system's clang (ENT_GPU_CC): the environment's own sysroot
has an older C library than the system's libcuda needs.
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
HOST = os.path.join(ROOT, "bench", "bench_main.c")
RUNTIME = os.path.join(HERE, "gpu_runtime.c")

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
VARIANTS = ["fused", "fused-omp", "gpu-managed", "gpu-host"]


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

    # Every entity loop on the GPU, however few entities it has.
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
         "-o", os.path.join(OUT, "gpu")])


def measure(variant, n, threads, target_ms):
    exe = os.path.join(OUT, "gpu" if variant.startswith("gpu") else variant)
    env = dict(os.environ, OMP_NUM_THREADS=str(threads))
    if variant == "gpu-host":
        env["ENT_GPU_MEM"] = "host"
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
