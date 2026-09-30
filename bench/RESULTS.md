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

## 2026-09-29: resources and a single-entity archetype in the example

Same machine and toolchain, commit dc7efd5. The example's frame now also
reads the wind strength from a resource (loaded once before the particle
loop), moves one player through a capacity-1 guard, and advances a frame
counter in a separate `tick` call after the fused loops. The C reference
does the same. All variants produced identical checksums at every size.
Load average 4.4 before, 3.4 between, 6.3 after; Spotlight's indexer had
used a full core shortly before the run and was down to 8% at the start.

### OpenMP runtime defaults (`KMP_BLOCKTIME=0`, passive wait)

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 331 (18%) | 5,412 (6%) | 50,514 (1%) | 507,784 (1%) | 5,138,269 (0%) |
| stages-omp | 62,031 (10%) | 66,837 (11%) | 103,162 (5%) | 481,062 (6%) | 4,359,974 (4%) |
| entities-omp | 334 (22%) | 5,389 (3%) | 50,521 (1%) | 399,761 (1%) | 2,241,739 (1%) |
| fused | 278 (10%) | 4,475 (2%) | 45,467 (2%) | 451,343 (1%) | 4,520,546 (0%) |
| fused-entities-omp | 285 (10%) | 4,522 (2%) | 45,182 (2%) | 203,531 (3%) | 1,839,318 (0%) |
| c-fused | 313 (13%) | 4,451 (2%) | 45,151 (4%) | 452,738 (1%) | 4,519,588 (1%) |
| c-fused-restrict | 279 (9%) | 4,507 (3%) | 45,154 (2%) | 453,638 (1%) | 4,527,093 (1%) |

### `KMP_BLOCKTIME=200`

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 332 (11%) | 5,443 (4%) | 50,895 (1%) | 508,364 (1%) | 5,138,675 (0%) |
| stages-omp | 2,274 (22%) | 7,214 (11%) | 46,495 (24%) | 476,429 (15%) | 4,662,516 (9%) |
| entities-omp | 323 (20%) | 5,520 (7%) | 50,787 (1%) | 80,654 (23%) | 2,062,403 (2%) |
| fused | 310 (17%) | 4,534 (3%) | 45,209 (3%) | 451,893 (1%) | 4,518,020 (1%) |
| fused-entities-omp | 285 (15%) | 4,510 (2%) | 45,393 (3%) | 76,677 (21%) | 1,850,619 (7%) |
| c-fused | 286 (16%) | 4,506 (3%) | 45,493 (4%) | 453,494 (0%) | 4,546,172 (2%) |
| c-fused-restrict | 293 (14%) | 4,493 (3%) | 45,214 (1%) | 452,757 (1%) | 4,528,646 (2%) |

### What holds

- The resource read, the player guard and the extra `tick` call cost
  nothing measurable: every fused variant is within 2% of the previous
  run (fused 4.57 to 4.52 ms at 1e7, fused parallel 1.84 ms both times),
  below what these runs can resolve. Generated code still matches the C
  reference within 1%.
- Whether loading the resource once before the loop matters is not
  measured: there is no build without the hoist to compare against.

### Measured, not explained

- The earlier finding that `KMP_BLOCKTIME=200` makes 1e7 parallel slower
  and noisier than runtime defaults did not reproduce: fused parallel took
  1.85 ms (spread 7%) against 1.84 ms with defaults, and entities-omp was
  faster (2.06 vs 2.24 ms). The three earlier runs had more background
  load; whether that explains them is not verified.

## 2026-09-29: storage for an optional component under churn

Same machine and toolchain. `bench/churn/`: N entities with Position and
Velocity; a fraction also holds Status (a timer). Each frame, churn * N / 2
holders lose Status and as many others gain it (uniformly at random), then
`move` runs on everyone and `status` (slow dx, count the timer down) on the
holders. The changes are precomputed and identical for every variant; all
variants agree on the checksum in every configuration. Single-threaded;
median of 5 processes, each the best of 5 repetitions of 64 frames; load
average 1.9 before and 2.4 after.

Variants: **archetypes** (two tables; a change copies the entity to the
other table and swap-removes it), **wide-select** (one table with a Status
column and a presence byte; `status` computes for everyone and selects,
branch-free, as the compiler would emit it), **wide-branch** (the same, with
an `if`), **sparse-set** (Status packed densely with its owner's id; `status`
reaches Velocity through the id).

### n = 1e6: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set |
|---|---|---|---|---|---|
| 1% | 0.0% | **171.7 (3%)** | 312.3 (2%) | 449.0 (6%) | 176.3 (4%) |
| 1% | 0.1% | 184.1 (5%) | 313.2 (3%) | 456.0 (7%) | **179.4 (3%)** |
| 1% | 1.0% | 282.9 (8%) | 322.6 (1%) | 473.2 (7%) | **190.1 (2%)** |
| 10% | 0.0% | **181.7 (2%)** | 311.5 (2%) | 739.3 (8%) | 226.6 (3%) |
| 10% | 0.1% | **197.2 (5%)** | 315.5 (2%) | 803.5 (8%) | 235.2 (4%) |
| 10% | 1.0% | 301.6 (10%) | 325.3 (2%) | 850.8 (9%) | **285.0 (4%)** |
| 10% | 10.0% | 1,332.9 (10%) | **426.1 (4%)** | 948.7 (5%) | 486.4 (2%) |
| 50% | 0.0% | **225.8 (2%)** | 311.6 (1%) | 2,498.7 (6%) | 367.1 (2%) |
| 50% | 0.1% | **241.0 (2%)** | 313.9 (1%) | 2,562.3 (4%) | 376.8 (2%) |
| 50% | 1.0% | 355.1 (5%) | **322.4 (2%)** | 2,605.7 (4%) | 484.5 (1%) |
| 50% | 10.0% | 1,452.3 (5%) | **429.0 (2%)** | 2,737.4 (2%) | 804.0 (2%) |
| 90% | 0.0% | **269.9 (2%)** | 312.3 (3%) | 794.9 (6%) | 505.2 (3%) |
| 90% | 0.1% | **289.1 (6%)** | 314.0 (1%) | 952.9 (2%) | 517.5 (3%) |
| 90% | 1.0% | 390.6 (5%) | **322.8 (3%)** | 985.3 (3%) | 642.1 (2%) |
| 90% | 10.0% | 1,499.3 (8%) | **429.6 (3%)** | 1,076.8 (2%) | 1,082.9 (3%) |

Churn / systems split, us per frame:

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set |
|---|---|---|---|---|---|
| 1% | 1.0% | 113.5 / 170.4 | 9.5 / 313.1 | 9.4 / 463.8 | 13.8 / 176.2 |
| 10% | 10.0% | 1,156.0 / 176.8 | 95.6 / 330.6 | 90.7 / 857.9 | 209.4 / 274.6 |
| 50% | 1.0% | 130.7 / 222.2 | 9.6 / 312.9 | 9.6 / 2,596.3 | 21.6 / 463.4 |
| 50% | 10.0% | 1,229.1 / 223.8 | 96.0 / 331.9 | 93.0 / 2,644.4 | 181.7 / 620.3 |
| 90% | 10.0% | 1,217.7 / 280.8 | 95.7 / 331.9 | 94.4 / 982.9 | 177.1 / 905.2 |

Reserved memory: archetypes 49 MB (both tables need full capacity),
wide 21 MB, sparse-set 28 MB.

### n = 1e5: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set |
|---|---|---|---|---|---|
| 1% | 0.0% | **16.4 (58%)** | 30.5 (68%) | 44.3 (63%) | 17.0 (83%) |
| 1% | 0.1% | 17.3 (45%) | 30.8 (27%) | 45.0 (11%) | **17.3 (8%)** |
| 1% | 1.0% | 23.6 (9%) | 31.2 (5%) | 45.8 (7%) | **17.9 (9%)** |
| 10% | 0.0% | **17.6 (10%)** | 30.2 (9%) | 47.7 (9%) | 21.7 (10%) |
| 10% | 0.1% | **18.0 (6%)** | 30.1 (9%) | 51.1 (4%) | 21.9 (6%) |
| 10% | 1.0% | 25.4 (12%) | 30.6 (10%) | 77.3 (9%) | **23.9 (11%)** |
| 10% | 10.0% | 152.9 (10%) | 36.2 (10%) | 90.2 (9%) | **33.5 (8%)** |
| 50% | 0.0% | **21.8 (12%)** | 30.5 (3%) | 94.4 (86%) | 35.5 (8%) |
| 50% | 0.1% | **23.1 (6%)** | 30.0 (10%) | 145.1 (10%) | 35.7 (9%) |
| 50% | 1.0% | **30.8 (8%)** | 31.1 (7%) | 234.4 (8%) | 42.5 (6%) |
| 50% | 10.0% | 156.5 (11%) | **36.1 (7%)** | 259.9 (8%) | 78.5 (10%) |
| 90% | 0.0% | **26.0 (16%)** | 30.4 (12%) | 49.5 (7%) | 49.1 (8%) |
| 90% | 0.1% | **27.2 (9%)** | 30.4 (4%) | 61.5 (5%) | 50.3 (8%) |
| 90% | 1.0% | 34.5 (10%) | **31.3 (10%)** | 95.8 (7%) | 57.7 (4%) |
| 90% | 10.0% | 157.9 (18%) | **36.0 (9%)** | 99.3 (6%) | 109.4 (9%) |

### What holds

- No storage wins everywhere; churn rate and density decide.
  - Churn up to 0.1% of entities per frame: archetype moves win except
    at 1% density, where they tie with sparse-set.
  - Churn of 1%: sparse-set wins at 1-10% density, wide-select at 50-90%.
  - Churn of 10%: wide-select wins at every density, 3.1-3.5x faster than
    archetype moves at 1e6.
- The split gives the cost model behind it, at n=1e6:
  - archetypes: a status pass over the holders only, plus 11-13 ns per
    change (copy and swap-remove at random rows);
  - wide-select: a full pass (about 312 us whatever the density), plus
    about 1 ns per change;
  - sparse-set: about 2 ns per change, but its status pass reaches
    Velocity through the owner's id, and grows faster with density than a
    contiguous pass (505 us at 90%).
- Branch-free code matters: the wide archetype with an `if` is 2-8x slower
  than the select form at 10-50% density (2.5 ms against 0.31 ms at 50%).
  The compiler can emit the select form because query bodies are
  entity-local and every slot is valid memory; a C programmer's natural
  `if` is the slow one.
- With capacities, archetype moves reserve full capacity in both tables:
  49 MB against 21 MB (wide) and 28 MB (sparse-set) here.

So the storage of an optional component should be a per-component
decision, and the measured costs are enough for a first rule of thumb.

### Limits and not explained

- Single-threaded; one optional component; changes uniformly random (the
  worst case for archetype moves, whose copies then miss the cache).
  With k optional components, archetype moves would also split entities
  over up to 2^k tables; not measured.
- At n=1e5, the configurations without churn at 1% density are noisy
  (58-83% spread) for every variant; why is not known. The rest of the
  1e5 table follows the 1e6 pattern.

## 2026-09-29: the compiled program in the churn benchmark

Same machine and toolchain, commit c3ef71a. `bench/churn/run.py` now also
runs `bench/churn/status.mlir`, where Status is an optional component,
lowered per system ("compiled") and fused ("compiled-fused"); the host
applies the same precomputed changes through the generated header. All
six variants agree on the checksum in every configuration.

Spotlight's indexer used 88-99% of one core throughout (load average 7.7
before, 4.0 after). The benchmark is single-threaded, and the four
hand-written variants reproduce the previous churn run within 1-2% at
n=1e6, so the load made no measurable difference there.

### n = 1e6: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **171.7 (3%)** | 312.5 (3%) | 452.2 (5%) | 177.0 (2%) | 284.3 (3%) | 256.5 (3%) |
| 1% | 0.1% | 186.0 (1%) | 313.9 (2%) | 464.4 (4%) | **179.6 (4%)** | 286.6 (3%) | 258.4 (2%) |
| 1% | 1.0% | 284.5 (3%) | 322.2 (1%) | 475.0 (5%) | **191.3 (3%)** | 294.6 (2%) | 267.1 (3%) |
| 10% | 0.0% | **181.5 (1%)** | 312.5 (1%) | 743.9 (4%) | 226.2 (1%) | 284.4 (3%) | 257.0 (2%) |
| 10% | 0.1% | **197.3 (3%)** | 313.9 (0%) | 797.2 (2%) | 235.7 (3%) | 285.5 (2%) | 258.5 (1%) |
| 10% | 1.0% | 304.4 (2%) | 323.3 (1%) | 847.9 (5%) | 287.6 (2%) | 294.2 (2%) | **267.2 (1%)** |
| 10% | 10.0% | 1,356.9 (6%) | 425.6 (2%) | 952.1 (6%) | 483.9 (4%) | 381.6 (2%) | **354.4 (3%)** |
| 50% | 0.0% | **226.6 (1%)** | 312.7 (1%) | 2,500.9 (4%) | 367.4 (1%) | 284.7 (3%) | 256.7 (2%) |
| 50% | 0.1% | **245.7 (1%)** | 314.8 (1%) | 2,570.2 (3%) | 377.6 (0%) | 285.2 (1%) | 258.1 (1%) |
| 50% | 1.0% | 357.0 (2%) | 323.0 (0%) | 2,624.8 (4%) | 485.2 (1%) | 295.7 (1%) | **267.3 (2%)** |
| 50% | 10.0% | 1,452.4 (3%) | 425.9 (2%) | 2,725.9 (3%) | 792.8 (2%) | 378.2 (3%) | **353.7 (3%)** |
| 90% | 0.0% | 270.7 (1%) | 312.7 (1%) | 806.8 (3%) | 503.8 (1%) | 284.6 (2%) | **257.0 (1%)** |
| 90% | 0.1% | 290.1 (1%) | 314.7 (1%) | 948.8 (2%) | 517.2 (1%) | 286.4 (2%) | **258.4 (3%)** |
| 90% | 1.0% | 398.4 (3%) | 323.5 (2%) | 979.7 (1%) | 635.3 (2%) | 295.1 (1%) | **267.9 (1%)** |
| 90% | 10.0% | 1,479.6 (2%) | 426.4 (2%) | 1,074.6 (1%) | 1,085.8 (2%) | 380.7 (1%) | **354.7 (1%)** |

Systems time alone: compiled 283-286 us and compiled-fused 256-258 us at
every configuration, against 312-330 us for the hand-written select form.

### n = 1e5: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **16.3 (103%)** | 30.6 (25%) | 44.1 (35%) | 17.2 (18%) | 28.3 (46%) | 25.6 (79%) |
| 1% | 0.1% | **17.4 (58%)** | 30.9 (28%) | 44.9 (19%) | 17.5 (9%) | 28.6 (3%) | 25.4 (7%) |
| 1% | 1.0% | 23.5 (6%) | 31.4 (3%) | 45.9 (8%) | **17.9 (11%)** | 28.8 (4%) | 25.9 (3%) |
| 10% | 0.0% | **17.2 (11%)** | 30.6 (2%) | 48.2 (6%) | 21.5 (9%) | 27.8 (4%) | 25.0 (10%) |
| 10% | 0.1% | **18.4 (3%)** | 30.4 (6%) | 51.3 (3%) | 22.1 (10%) | 27.8 (7%) | 25.4 (7%) |
| 10% | 1.0% | 25.6 (8%) | 31.4 (2%) | 76.1 (6%) | **23.9 (7%)** | 28.6 (2%) | 25.8 (5%) |
| 10% | 10.0% | 150.4 (11%) | 36.2 (4%) | 87.8 (7%) | 33.2 (7%) | 33.8 (3%) | **30.7 (4%)** |
| 50% | 0.0% | **22.0 (2%)** | 30.6 (5%) | 98.9 (66%) | 35.5 (3%) | 27.7 (11%) | 24.9 (3%) |
| 50% | 0.1% | **23.2 (7%)** | 29.8 (5%) | 147.4 (4%) | 36.4 (8%) | 27.8 (10%) | 24.6 (7%) |
| 50% | 1.0% | 30.9 (6%) | 31.2 (5%) | 242.3 (7%) | 41.7 (9%) | 28.3 (8%) | **26.4 (6%)** |
| 50% | 10.0% | 160.6 (8%) | 35.9 (5%) | 261.9 (5%) | 80.5 (5%) | 33.6 (4%) | **31.4 (11%)** |
| 90% | 0.0% | 26.5 (5%) | 30.3 (1%) | 50.0 (2%) | 48.9 (3%) | 27.9 (8%) | **25.3 (6%)** |
| 90% | 0.1% | 27.4 (5%) | 30.3 (7%) | 62.3 (4%) | 50.4 (6%) | 27.8 (8%) | **24.6 (6%)** |
| 90% | 1.0% | 34.7 (3%) | 30.8 (6%) | 97.2 (4%) | 56.6 (7%) | 28.6 (7%) | **26.0 (8%)** |
| 90% | 10.0% | 159.7 (2%) | 35.9 (7%) | 100.2 (1%) | 109.4 (7%) | 33.6 (5%) | **31.5 (10%)** |

### What holds

- The compiler's masked, branch-free lowering of an optional component is
  faster than the same form written by hand: 9% per system (284 against
  312 us at 1e6) and 18% fused (257 us), at every density.
- The compiled wide form now wins most configurations at 1e6: every
  churn rate at 90% density, from 1% churn at 10-50% density, and 10%
  churn everywhere, where it is about 4x faster than archetype moves (354
  against 1,357-1,480 us). Archetype moves still win at churn up to 0.1%
  with 10-50% density, and sparse-set at 1% density with churn. A
  per-component choice remains necessary.

### Why the compiled form is 9% faster: vectorisation width

Checked afterwards with three back-to-back process triples per density
at n=1e6 without churn (load average 2-4), systems time:

| density | hand-written select | with `-DSTAGGER` | with `-DWIDTH16` | compiled |
|---|---|---|---|---|
| 10% | 294-298 us | 293-307 us | – | 270-278 us |
| 90% | 298-303 us | 296-316 us | – | 271-290 us |
| 10% | 309-323 us | – | 289-302 us | 281-297 us |
| 90% | 310-314 us | – | 289-290 us | 280-285 us |

(The two halves are separate runs.) Staggered allocations do not change
the hand-written form, so staggering is not the reason. The vectoriser is:
LLVM runs the compiled status loop 16 entities wide but the hand-written
one 4 wide with 2x interleave (`-Rpass=loop-vectorize`), and forcing the
hand-written loop to width 16 recovers all but 2-3% of the gap, which is
within the noise of these runs. The compiled body compares the presence
byte as a byte (`cmpi ne` on i8), while the C source widens it to `int`
first; the narrow type is the likely reason LLVM picks the wider factor,
not verified beyond that.

### Measured, not explained

- At n=1e5, the configurations without churn at 1% density are noisy
  again (46-103% spread), for every variant, as in the first churn run.

## 2026-09-30: entity ids and moves (commit d449dcd)

Same machine and toolchain. Every archetype now has an id column and the
world an entity table; hosts spawn through the generated header. The
frame loops themselves only changed offsets. All variants of both
benchmarks agree on their checksums.

Not a quiet run: during the main benchmark a Bazel clang++ used a full
core and Spotlight 85% of one (load average 8.5 falling to 4.3); during
the churn benchmark a Mojo kgen process used about 40% of one core (load
average 4.3 to 4.5).

### Main benchmark, OpenMP runtime defaults

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 317 (21%) | 5,529 (3%) | 51,028 (1%) | 514,878 (1%) | 5,200,132 (2%) |
| stages-omp | 64,337 (9%) | 70,641 (8%) | 109,200 (5%) | 496,896 (11%) | 4,430,080 (6%) |
| entities-omp | 335 (16%) | 5,428 (3%) | 51,449 (2%) | 428,056 (9%) | 2,271,724 (3%) |
| fused | 274 (15%) | 4,541 (2%) | 45,667 (3%) | 455,456 (2%) | 4,563,546 (2%) |
| fused-entities-omp | 265 (9%) | 4,615 (4%) | 45,843 (2%) | 210,581 (10%) | 1,851,090 (1%) |
| c-fused | 303 (10%) | 4,495 (3%) | 45,377 (2%) | 454,464 (3%) | 4,566,698 (2%) |
| c-fused-restrict | 265 (17%) | 4,495 (3%) | 45,896 (3%) | 462,962 (2%) | 4,659,126 (6%) |

### Churn benchmark, n = 1e6: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **172.3 (2%)** | 316.4 (3%) | 469.5 (3%) | 178.4 (2%) | 289.4 (2%) | 261.0 (3%) |
| 1% | 0.1% | 191.4 (6%) | 317.9 (2%) | 474.9 (4%) | **182.5 (4%)** | 289.5 (3%) | 261.6 (3%) |
| 1% | 1.0% | 301.4 (11%) | 327.6 (1%) | 496.8 (4%) | **194.7 (4%)** | 299.1 (2%) | 270.8 (2%) |
| 10% | 0.0% | **183.8 (3%)** | 316.8 (2%) | 763.4 (5%) | 229.4 (1%) | 288.4 (2%) | 261.0 (1%) |
| 10% | 0.1% | **203.6 (4%)** | 318.4 (3%) | 839.8 (5%) | 239.6 (2%) | 291.3 (2%) | 262.3 (1%) |
| 10% | 1.0% | 318.5 (10%) | 328.1 (2%) | 899.0 (3%) | 291.1 (3%) | 302.1 (2%) | **272.8 (3%)** |
| 10% | 10.0% | 1,475.8 (14%) | 429.4 (2%) | 992.9 (1%) | 493.7 (6%) | 389.0 (2%) | **359.3 (1%)** |
| 50% | 0.0% | **229.6 (2%)** | 316.4 (2%) | 2,619.4 (2%) | 372.6 (3%) | 288.8 (2%) | 260.6 (2%) |
| 50% | 0.1% | **250.3 (5%)** | 317.0 (2%) | 2,676.1 (2%) | 382.8 (4%) | 287.9 (2%) | 260.6 (2%) |
| 50% | 1.0% | 378.1 (9%) | 328.2 (2%) | 2,740.4 (2%) | 496.9 (3%) | 299.0 (2%) | **271.2 (2%)** |
| 50% | 10.0% | 1,550.8 (4%) | 433.8 (2%) | 2,863.6 (2%) | 813.0 (2%) | 386.4 (1%) | **358.8 (4%)** |
| 90% | 0.0% | 274.2 (2%) | 318.1 (2%) | 815.6 (12%) | 512.9 (1%) | 291.2 (13%) | **262.8 (5%)** |
| 90% | 0.1% | 299.2 (8%) | 320.8 (5%) | 970.4 (4%) | 529.5 (5%) | 291.8 (4%) | **264.4 (8%)** |
| 90% | 1.0% | 420.5 (22%) | 330.2 (4%) | 1,005.7 (5%) | 660.8 (12%) | 300.4 (9%) | **275.5 (9%)** |
| 90% | 10.0% | 1,707.4 (21%) | 426.5 (3%) | 1,088.2 (2%) | 1,124.2 (4%) | 387.8 (2%) | **360.6 (2%)** |

### What holds

- Ids cost the frame nothing measurable. Main benchmark against the
  2026-09-29 resources run: fused +0.4% to +1.5% at every size (4.52 to
  4.56 ms at 1e7), fused parallel +0.7% at 1e7 (+3.5% at 1e6, with a 10%
  spread). Churn against the previous churn run: the compiled variants
  are 1.6-1.8% slower, but so are the hand-written ones, which contain
  no id code (wide-select +1.3%, sparse-set +1%); the background load is
  the likelier cause. The winners per configuration are unchanged.
- Ids do cost memory: the compiled churn world reserves 45 MB at 1e6
  entities instead of 21 MB, 8 bytes of id column and 16 bytes of entity
  table per entity. Creating a world touches none of it, but spawning
  writes each entity's id and table entry, so for live entities it is
  resident.

## 2026-09-30: compact entity ids (commit c2e9a66)

Same machine and toolchain. The world now picks the id scheme from the
program's structural changes: both benchmark programs neither despawn nor
move entities, so their ids are archetype and row, with no id column and no
entity table. All variants of both benchmarks agree on their checksums. A
quiet run: load average 3.9 falling to 2.5; a virtual machine used 12-20%
of one core.

### Main benchmark, OpenMP runtime defaults

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 321 (8%) | 5,441 (2%) | 51,078 (2%) | 510,376 (1%) | 5,179,833 (2%) |
| stages-omp | 63,804 (1%) | 68,017 (1%) | 104,267 (4%) | 491,118 (5%) | 4,413,809 (1%) |
| entities-omp | 327 (16%) | 5,523 (7%) | 50,735 (2%) | 408,897 (2%) | 2,264,414 (0%) |
| fused | 304 (11%) | 4,479 (1%) | 45,424 (2%) | 455,117 (1%) | 4,536,710 (0%) |
| fused-entities-omp | 280 (12%) | 4,528 (3%) | 45,177 (1%) | 210,555 (2%) | 1,842,731 (1%) |
| c-fused | 280 (6%) | 4,554 (3%) | 45,090 (1%) | 454,434 (1%) | 4,545,506 (0%) |
| c-fused-restrict | 274 (13%) | 4,508 (2%) | 45,304 (3%) | 453,802 (0%) | 4,552,044 (1%) |

### Churn benchmark, n = 1e6: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **171.6 (1%)** | 313.1 (0%) | 462.9 (0%) | 177.9 (0%) | 285.2 (1%) | 258.1 (1%) |
| 1% | 0.1% | 186.0 (2%) | 313.9 (1%) | 468.2 (1%) | **180.4 (1%)** | 285.7 (0%) | 258.7 (1%) |
| 1% | 1.0% | 284.5 (1%) | 323.0 (1%) | 486.3 (1%) | **191.2 (2%)** | 295.3 (4%) | 268.2 (2%) |
| 10% | 0.0% | **182.4 (1%)** | 313.0 (1%) | 771.4 (8%) | 226.3 (2%) | 285.1 (2%) | 257.8 (2%) |
| 10% | 0.1% | **198.4 (3%)** | 314.1 (2%) | 825.4 (1%) | 236.1 (0%) | 286.2 (1%) | 258.7 (2%) |
| 10% | 1.0% | 305.1 (3%) | 323.2 (1%) | 880.4 (1%) | 288.9 (1%) | 295.3 (0%) | **268.3 (2%)** |
| 10% | 10.0% | 1,372.7 (1%) | 429.0 (1%) | 978.2 (1%) | 491.6 (1%) | 382.0 (0%) | **354.9 (1%)** |
| 50% | 0.0% | **226.9 (1%)** | 312.9 (1%) | 2,574.8 (1%) | 366.6 (0%) | 284.9 (0%) | 257.0 (0%) |
| 50% | 0.1% | **245.4 (1%)** | 314.5 (0%) | 2,644.5 (1%) | 377.9 (1%) | 286.4 (0%) | 258.6 (0%) |
| 50% | 1.0% | 361.3 (1%) | 323.0 (0%) | 2,705.3 (0%) | 486.6 (1%) | 295.3 (0%) | **268.2 (0%)** |
| 50% | 10.0% | 1,485.2 (1%) | 428.8 (1%) | 2,817.0 (0%) | 800.3 (1%) | 382.5 (0%) | **354.8 (0%)** |
| 90% | 0.0% | 271.4 (1%) | 312.6 (0%) | 791.3 (3%) | 506.1 (0%) | 284.9 (0%) | **257.1 (1%)** |
| 90% | 0.1% | 290.2 (1%) | 314.2 (1%) | 950.9 (1%) | 516.7 (0%) | 286.5 (0%) | **259.1 (1%)** |
| 90% | 1.0% | 395.5 (0%) | 322.6 (0%) | 981.4 (0%) | 638.4 (1%) | 294.9 (0%) | **267.1 (1%)** |
| 90% | 10.0% | 1,498.1 (1%) | 427.9 (0%) | 1,079.9 (0%) | 1,089.7 (1%) | 381.9 (0%) | **354.8 (0%)** |

### What holds

- Compact ids cost no frame time. Main benchmark: fused 4.54 ms at 1e7
  (4.52 before entity ids, 4.56 with uncompacted ids), fused parallel
  1.84 ms, all within 0.6%. Churn at 1e6: compiled 285 us and compiled-fused
  257-258 us per frame, the same as before entity ids existed; the winners
  per configuration are unchanged.
- They give the memory back: the compiled churn world reserves 21 MB at
  1e6 entities again, down from 45 MB with uncompacted ids, since neither
  benchmark program despawns or moves entities. For a program that does,
  32-bit generational ids cost 10 bytes per entity at 1e6 (measured from
  the layout: id 4, generation 2, location 4) instead of 24.
