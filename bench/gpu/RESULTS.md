# Systems on the GPU: timings

`python3 bench/gpu/run.py` (see its docstring for the variants): the
integrate benchmark's frame (n bodies and n particles, fused) on one core,
with OpenMP, and on the GPU. `python3 bench/gpu/run_churn.py`: what it
costs when the CPU touches what the GPU goes through. `probe_launch.c` and
`probe_readback.c`: the driver alone.

## 2026-10-17: G1 (docs/gpu.md), steps 1 to 3 built

The machine of the entry below, idle this time (load average 0.5 to 2).
LLVM 22.1.8, `-O2 -march=native`; median over 3 rounds, each the best of 3
repetitions of about 200 ms; 8 OpenMP threads.

ns per frame, in us (`gpu`: the language's `gpu system`s built as `ent
build --gpu` builds them; `g0`: the prototype of the entry below):

| entities | fused | fused-omp | g0 | gpu | gpu-host |
|---|---|---|---|---|---|
| 10,000 | 1.7 | 1.7 | 14.7 | 11.3 | 47.6 |
| 100,000 | 29.5 | 28.7 | 18.0 | 12.9 | 359.4 |
| 1,000,000 | 879.4 | 281.6 | 388.2 | 382.3 | 3,264.7 |
| 10,000,000 | 12,361.9 | 10,538.1 | 3,802.6 | 3,732.1 | 32,359.1 |

The same checksum everywhere.

### What holds

- G1's build is as fast as G0's where the data decides (1e6, 1e7), and
  its floor is 11 us instead of 15: one stream, no wait after each
  launch, one wait when the frame ends.
- What is left of the floor is the driver's: three kernels (integrate's
  frame has one per archetype: bodies, particles, the player) and one
  synchronize take 9.7 to 10.8 us with empty kernels
  (`probe_launch.c`: 0.2 us for the synchronize alone, then about 2.5 us
  a launch). Fewer launches a frame is what would lower it.
- On an idle machine OpenMP is 3.1x one core at 1e6 and faster than the
  GPU there (282 against 382 us); at 1e7 the CPU side is bound by memory
  (10.5 ms, 1.2x one core) and the GPU 2.8x OpenMP.
- The 64 KB layout (columns of what a `gpu` system visits at 64 KB
  boundaries) changes nothing measurable: built without it, the same
  sweep gave 11.1, 14.1, 383.4 and 3,675.0 us for `gpu`. The CPU writing
  a managed page every frame (the counts, `Clock`) next to the first
  column did not cost the GPU anything; nor did it in `probe_launch.c`.

### The CPU touching what the GPU goes through

`run_churn.py`, ms per frame, a million entities moved by a `gpu` system
(its 16 MB fit the card's L2, hence 0.04 ms), 300 frames:

| spawn a frame | CPU reads a column | cpu | gpu | gpu, advised |
|---|---|---|---|---|
| 0 | no | 0.203 | 0.039 | 0.038 |
| 1,000 | no | 0.267 | 1.207 | 0.265 |
| 0 | yes | 0.764 | 2.798 | 12.071 |
| 1,000 | yes | 0.957 | 15.536 | 13.758 |

The sums agree. "advised": the world advised to stay on the GPU, with the
CPU reaching it there (`ENT_GPU_ADVISE=device`, devices/gpu.c).

- With managed memory left to itself, spawning a thousand entities a
  frame costs 1.2 ms (the pages at the columns' ends go to the CPU and
  back), reading a column 2.8 ms, both 15.5 ms: slower than the CPU
  alone in every case.
- Advised to stay on the GPU, a spawn costs what it does on the CPU (the
  CPU writes over PCIe), but reading a column over PCIe takes 12 ms.
- `probe_readback.c` (a million floats the GPU adds to and the CPU then
  adds up, every frame): managed memory moved on faults 3.0 to 3.6 ms,
  prefetched both ways 1.7 to 2.6 ms, advised read-mostly 6.5 to 142 ms;
  the GPU's own memory copied to pinned host memory 0.92 ms (the CPU's
  adding up included). A column the CPU only reads need not go back,
  which managed memory cannot know.

So prefetch hints over managed memory would not make a program whose CPU
reads what the GPU writes competitive: copies the compiler places, from
the GPU's own memory to the CPU's and back only where the CPU wrote,
would be about 3x better.


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
