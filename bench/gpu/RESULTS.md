# Systems on the GPU: timings

`python3 bench/gpu/run.py` (see its docstring for how the program is
made): the integrate benchmark's frame (`examples/integrate.mlir`, n
bodies and n particles, fused) on one core, with OpenMP, and on the GPU.

## 2026-10-16: G0, the frame's loops as kernels, by upstream passes only

Linux (Arch), AMD Strix (Radeon 880M/890M graphics, 24 threads) with an
NVIDIA GeForce RTX 4050 Laptop GPU (6 GB, driver 610.57.04, `sm_89`),
LLVM 22.1.8, CUDA in `/opt/cuda`. Built `-O2 -march=native` with the
system's clang. Median over 3 rounds, each the best of 3 repetitions of
about 200 ms; 8 OpenMP threads.

The machine was not idle: load average 8.4 to 10 throughout (other work
on the CPU), which affects the CPU columns, OpenMP's most. On the M4 Max
an idle machine gave OpenMP 2.7x over one core at 1e7
(`bench/RESULTS.md`, 2026-09-28); here it gave 1.1x.

ns per frame, in us:

| entities | fused (one core) | fused-omp (8 threads) | gpu-managed | gpu-host |
|---|---|---|---|---|
| 10,000 | 2.7 | 2.5 | 16.1 | 51.5 |
| 100,000 | 43.5 | 49.8 | 19.9 | 346.3 |
| 1,000,000 | 1,310.1 | 1,061.5 | 388.9 | 3,003.6 |
| 10,000,000 | 14,474.6 | 12,781.5 | 3,752.1 | 29,541.8 |

Every variant gave the same checksum at every size: the kernels round as
the CPU does (separate `mul.rn` and `add.rn`, no contraction).

### What holds

- With the world on the GPU (`gpu-managed`), a frame of 1e7 takes 3.75
  ms, 3.9x one core. It moves about 640 MB a frame (16 floats read or
  written per body and particle together), about 170 GB/s, near what the card's
  memory gives: the frame is bound by memory on both sides.
- Where the GPU reaches the world over PCIe each frame (`gpu-host`), a
  frame takes twice as long as on one core (about 22 GB/s). So the
  columns have to stay on the GPU between frames: keeping them there is
  the design's core, not a refinement.
- A frame on the GPU costs at least about 16 us; it pays from about
  30,000 entities on (against one core).
- Nothing in the language or the compiler changed: ent-opt's parallel
  entity loops go through `scf-parallel-loop-tiling`,
  `gpu-map-parallel-loops`, `convert-parallel-loops-to-gpu`,
  `gpu-kernel-outlining` and `gpu-lower-to-nvvm-pipeline` as they are.

### Measured, not explained

- What the 16 us floor is made of. Candidates: the lowering creates and
  destroys a stream around each of the two launches; the host writes the
  counts, the clock and the player each frame, in the arena's first page,
  where the first column also starts, so that page may move between the
  CPU and the GPU every frame.
- OpenMP is no faster than one core at 1e5 because the loops' parallel
  version starts at 1e6 entities (the compiler's default threshold).
