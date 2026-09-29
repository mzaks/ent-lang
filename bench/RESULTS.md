# Benchmark results

## 2026-09-28: fusion and entity parallelism (M3 part 1)

Apple M4 Max (12 performance + 4 efficiency cores, 64 GB), Homebrew LLVM
22.1.8, `clang -O2`, `OMP_NUM_THREADS=12`. `python3 bench/run.py`: every
number is the median over 5 processes, each the best of 3 repetitions of
about 200 ms; processes run round-robin over sizes and variants. All
variants produced bit-identical checksums at every size.

The machine was not idle: macOS storage services used about 1.5 cores and
a Zoom call about 0.2 of a core throughout (load average 4 to 11). The
default-blocktime run reproduced an earlier run under heavier load within
a few percent.

ns per frame; one frame updates n bodies and n particles (and skips n
scenery entities). Spread = (max - min) / median over the 5 processes.

### OpenMP runtime defaults (`KMP_BLOCKTIME=0`, passive wait)

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 285 (2%) | 5,946 (2%) | 51,014 (2%) | 515,071 (2%) | 5,216,323 (1%) |
| stages-omp | 64,879 (3%) | 68,857 (9%) | 104,170 (7%) | 476,082 (4%) | 4,438,231 (1%) |
| entities-omp | 323,408 (8%) | 324,325 (4%) | 337,768 (4%) | 411,472 (4%) | 2,289,079 (1%) |
| fused | 289 (1%) | 5,059 (1%) | 49,284 (1%) | 497,226 (1%) | 4,965,371 (2%) |
| fused-entities-omp | 128,169 (9%) | 130,190 (3%) | 142,422 (2%) | 217,465 (3%) | 1,858,796 (1%) |
| c-fused | 288 (1%) | 5,055 (2%) | 48,958 (1%) | 495,850 (2%) | 4,974,483 (2%) |
| c-fused-restrict | 290 (1%) | 5,075 (2%) | 49,172 (1%) | 497,290 (2%) | 4,936,991 (1%) |

### `KMP_BLOCKTIME=200` (workers spin between regions)

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 294 (3%) | 5,905 (3%) | 50,963 (3%) | 512,841 (3%) | 5,218,334 (3%) |
| stages-omp | 2,960 (19%) | 7,709 (20%) | 57,022 (17%) | 503,696 (13%) | 5,051,298 (5%) |
| entities-omp | 11,741 (18%) | 12,672 (25%) | 22,128 (6%) | 110,047 (4%) | 2,399,052 (72%) |
| fused | 301 (6%) | 5,086 (2%) | 49,620 (3%) | 497,322 (1%) | 5,030,444 (2%) |
| fused-entities-omp | 5,156 (19%) | 5,536 (8%) | 13,544 (7%) | 87,782 (15%) | 2,322,835 (51%) |
| c-fused | 300 (6%) | 5,058 (2%) | 48,971 (3%) | 496,278 (10%) | 4,992,368 (1%) |
| c-fused-restrict | 302 (5%) | 5,081 (2%) | 49,021 (3%) | 492,009 (1%) | 4,949,575 (2%) |

### What holds

- The generated fused code matches hand-written C within 1% at every size;
  `restrict` changes nothing measurable.
- Fusion alone saves 15% at n=1e4 and 3-5% at the other sizes except 1e3,
  where it makes no difference. The gains are small, but reproduced in two
  runs.
- Entity-parallel loops need about 1e6 entities to pay off. Fused and
  parallel: 2.3x faster than fused at 1e6 and 2.7x at 1e7 with runtime
  defaults; 5.7x at 1e6 with `KMP_BLOCKTIME=200`.
- Fusion cuts the parallel regions per frame from 5 to 2, and the fused
  parallel variant is about 2.5x as fast as the unfused one below 1e6.
- Parallel stages (M2) are not worth it here: 15% at best (1e7), and far
  slower at small sizes. Entity parallelism wins at every size where
  parallelism pays at all.

### Measured, not explained

- With runtime defaults every fork costs about 65 us (about 130 us per frame
  for 2 regions), while an empty `#pragma omp parallel` in C costs 35 us
  with 12 threads. The extra wake-up in a work-sharing loop is a guess,
  not verified.
- At n=1e7 with `KMP_BLOCKTIME=200`, the parallel variants are slower and
  far noisier than with runtime defaults (fused parallel: 2.16-3.34 ms
  across processes vs 1.85-1.86 ms).
- From 1e5 to 1e7 the sequential cost per entity is flat (about 0.25 ns
  per entity update), although only 1e5 fits in cache. Which limit that
  is (single-core bandwidth is a candidate) is not measured.

## 2026-09-28: where the time goes, and whether layout helps (M3 part 2)

Same machine and toolchain. `bench/layout/`: the fused frame written by
hand in C in four layouts (SoA, AoS, AoSoA with blocks of 8 and 16), and a
read-modify-write kernel over K arrays (`streams.c`). All layouts agree on
the checksum. `python3 bench/layout/run.py streams|frame`: median of 5
processes; load average 3.4 at the start and 9 at the end.

### The generated loops are vectorized

The fused frame's loops run 4 entities per iteration (`fadd.4s`); in the
body loop, `dy` is reused from a register after the store instead of being
reloaded. Hand-written C compiles to the same loop.

### A single core runs at its stream bandwidth, in L2 and in DRAM alike

Read-modify-write bandwidth, one thread:

| streams K | 1 | 2 | 3 | 4 | 5 | 8 | 16 |
|---|---|---|---|---|---|---|---|
| 1 MB total (L2) | 142 | 143 | 131 | 128 | 124 | 90 | 10 |
| 160 MB total (DRAM) | 134 | 141 | 129 | 128 | 121 | 77 | 9 |
| 160 MB, 12 threads | 405 | 387 | 380 | 373 | 371 | 358 | 28 |

GB/s moved (read + write). The fused frame moves 64 bytes per entity pair
per frame (bodies read 4 columns and write 3, particles read 5 and write
4). At n=1e5 and at n=1e7 that is about 130 GB/s, which is the one-thread
rate for 4 to 5 streams in the table, in L2 and in DRAM alike. This
explains the flat cost per entity from 1e5 to 1e7 found in part 1: one core
is limited by how fast it can stream, not by where the data lives. With 12
threads, the fused parallel frame (1.8 ms at 1e7, about 355 GB/s) is close
to the 12-thread stream rate.

### AoS and AoSoA do not help

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| soa | 296 (8%) | 5,034 (19%) | 48,556 (1%) | 490,140 (1%) | 4,906,207 (2%) |
| aos | 688 (15%) | 8,262 (17%) | 82,657 (1%) | 831,178 (1%) | 8,350,993 (2%) |
| aosoa8 | 257 (21%) | 5,203 (11%) | 52,276 (2%) | 526,566 (3%) | 5,585,009 (3%) |
| aosoa16 | 255 (22%) | 4,710 (17%) | 46,671 (1%) | 474,019 (1%) | 5,403,020 (5%) |
| soa-omp | 64,106 (6%) | 65,268 (63%) | 77,930 (2%) | 149,101 (1%) | 1,796,304 (2%) |
| aos-omp | 63,029 (185%) | 67,919 (76%) | 84,045 (3%) | 234,116 (3%) | 2,221,854 (6%) |
| aosoa8-omp | 62,429 (252%) | 67,732 (81%) | 83,957 (1%) | 233,888 (177%) | 2,196,365 (4%) |
| aosoa16-omp | 63,968 (70%) | 64,895 (9%) | 76,148 (2%) | 140,739 (3%) | 1,940,117 (10%) |

ns per frame. AoS is 1.7x slower on one thread: it moves the unused
kg/lifetime field of every body along with the rest. AoSoA would only pay
if fewer streams meant more bandwidth, but up to 5 streams the rate barely
drops (134 to 121 GB/s), so AoSoA8 is 7-14% slower than SoA and AoSoA16
between 4% faster and 10% slower. With threads, SoA is best at 1e6 and
1e7. Small sizes with threads are dominated by fork cost and noise.

Conclusion: no AoSoA pass for now; SoA stays. The picture would change for
archetypes whose queries stream many more columns, but see below: the drop
at 8 and 16 streams is mostly an artefact of addresses.

### What does matter: where columns start

Large allocations come back page aligned (4 MB aligned at 40 MB), so every
column starts at the same cache set. Offsetting each array by 17 more
cache lines than the previous one (`-DSTAGGER`), three back-to-back
process pairs each:

| | plain | staggered |
|---|---|---|
| streams K=8, 160 MB, 1 thread | 56-70 GB/s | 101-109 GB/s |
| streams K=16, 160 MB, 1 thread | 8-9 GB/s | 65-84 GB/s |
| SoA frame n=1e5, 1 thread | 48.1-48.6 us | 44.6-44.8 us |
| SoA frame n=1e6, 1 thread | 489-491 us | 449 us |
| SoA frame n=1e7, 1 thread | 4.89-4.91 ms | 4.52-4.55 ms |
| SoA frame n=1e6, 12 threads | 151-154 us | 147-150 us |
| SoA frame n=1e7, 12 threads | 1.82-1.93 ms | 1.84-1.94 ms |

Staggered columns make the single-thread frame 7-8% faster at every size,
more than fusion did, and turn the collapse at 16 streams into a gradual
decline. With 12 threads, where DRAM bandwidth is the limit, it makes no
measurable difference. That columns starting at the same set conflict in
the cache is consistent with the addresses and with the fix working; the
M4's cache geometry (associativity) is not measured here.

The host allocates the columns today, so the compiler cannot do this. It
is the strongest argument so far for the language to own the world's
storage.

## 2026-09-29: compiler-owned world storage (arena, capacities)

Same machine and toolchain, commit 9164ba9: the world is one arena laid
out by the compiler with staggered columns; entity loops with
`parallel-entities` run in parallel only from `parallel-min-entities`
(default 1e6) entities on, and archetypes with smaller capacity never get
a parallel loop. The example's capacity is 1e7 per archetype, so every
size below is versioned on the count. All variants produced identical
checksums at every size.

The quietest run so far: load average 2.8 before, 4.3 between, 2.8 after;
no heavy process (WindowServer, Slack and Docker at under half a core
together).

### OpenMP runtime defaults (`KMP_BLOCKTIME=0`, passive wait)

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 343 (8%) | 5,448 (5%) | 51,518 (1%) | 513,713 (1%) | 5,183,432 (1%) |
| stages-omp | 63,223 (5%) | 67,436 (2%) | 104,203 (3%) | 484,569 (3%) | 4,423,657 (2%) |
| entities-omp | 322 (16%) | 5,461 (3%) | 50,874 (4%) | 404,334 (1%) | 2,260,857 (1%) |
| fused | 304 (12%) | 4,551 (2%) | 45,893 (3%) | 455,232 (0%) | 4,567,814 (0%) |
| fused-entities-omp | 275 (16%) | 4,490 (2%) | 45,455 (2%) | 207,962 (1%) | 1,842,330 (0%) |
| c-fused | 280 (11%) | 4,538 (2%) | 45,616 (1%) | 455,786 (1%) | 4,563,140 (0%) |
| c-fused-restrict | 282 (11%) | 4,566 (1%) | 45,532 (2%) | 454,995 (1%) | 4,557,531 (0%) |

### `KMP_BLOCKTIME=200`

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 348 (16%) | 5,472 (5%) | 51,394 (2%) | 515,515 (1%) | 5,219,797 (2%) |
| stages-omp | 2,739 (25%) | 7,563 (15%) | 53,766 (21%) | 497,913 (20%) | 4,843,400 (6%) |
| entities-omp | 333 (16%) | 5,510 (6%) | 51,543 (1%) | 90,649 (25%) | 2,442,367 (59%) |
| fused | 288 (14%) | 4,525 (2%) | 45,528 (2%) | 455,951 (1%) | 4,564,246 (2%) |
| fused-entities-omp | 298 (14%) | 4,549 (2%) | 45,207 (2%) | 85,588 (24%) | 2,212,380 (25%) |
| c-fused | 287 (7%) | 4,582 (2%) | 45,485 (2%) | 457,990 (2%) | 4,580,959 (1%) |
| c-fused-restrict | 287 (7%) | 4,567 (2%) | 46,041 (2%) | 457,976 (2%) | 4,572,941 (1%) |

### Compared with 2026-09-28 (host-allocated columns), runtime defaults

- Staggered columns deliver what the layout measurement predicted for the
  fused frame: 8.0% faster at 1e7 (4.97 to 4.57 ms), 8.4% at 1e6, 6.9% at
  1e5, 10% at 1e4. The C reference, now on the same arena, gains the same;
  generated fused code still matches it within 0.5%.
- The parallel threshold removes the fork cost below 1e6: fused parallel
  goes from 128 us to 0.28 us per frame at 1e3, from 130 us to 4.5 us at
  1e4 and from 142 us to 45 us at 1e5, where it now equals plain fused.
  From 1e6 on it runs in parallel as before: 208 us at 1e6 (2.2x faster
  than fused; 4% faster than before) and 1.84 ms at 1e7 (unchanged within
  1%).
- fused-entities-omp is now the fastest variant or tied for fastest at
  every size, so one build serves all sizes.

### Measured, not explained

- The unfused per-system loops do not gain from staggering: -0.6% at 1e7,
  -0.3% at 1e6, +1% at 1e5 (only 1e4 improves, by 9%), while the fused
  loops gain 7-8%. That loops streaming fewer columns suffer less from set
  conflicts would fit, but is not verified.
- With `KMP_BLOCKTIME=200`, 1e7 parallel is again slower and noisier than
  with runtime defaults (2.21 ms, spread 25%, vs 1.84 ms, 0%), as in both
  earlier runs.
- With `KMP_BLOCKTIME=200`, parallel loops would already pay at 1e5 (13.5
  us vs 49 us on 2026-09-28); the default threshold of 1e6 is tuned for
  the runtime's default settings and leaves that on the table.
