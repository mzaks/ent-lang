# Benchmark results

Entries before 2026-10-01's rename keep the names of the time: the language
was ecs-lang, its tools `ecs-opt` and `ecs-translate`, its passes and
attributes `--ecs-...` and `ecs.`, its generated C `ecs_...`; they are now
ent-lang, `ent-opt`, `ent-translate`, `--ent-...`, `ent.` and `ent_...`.

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

## 2026-09-30: cross-entity writes with ecs.apply (commit 05158c8)

Same machine (M4 Max, 16 cores) and toolchain (Homebrew clang/LLVM
22.1.8, OpenMP runtime defaults). `bench/apply/run.py`: N guns each deal
damage (1-4, so f32 sums are exact in any order) to one of M ships, chosen
at random with a fixed seed; a frame applies every gun once. Every variant
agrees on the checksum in every configuration. Load average 2.2 before the
runs; during them it rose to 11-16, with nothing but the benchmark visible
in `ps` (its OpenMP variants run 16 threads, and the atomic one runs for
seconds per process), so it is likely self-inflicted but not proven.

- `c-index`: hand-written C, `hp[t[i]] -= d[i]`, targets are rows,
  sequential;
- `c-atomic-par`: the same in an OpenMP loop with an atomic float add
  (compare-and-swap): parallel, order not fixed;
- `c-buffered`: what the compiled program does, without ids: an OpenMP
  loop fills (row, value) per gun, one sequential loop combines;
- `compiled-rows`: `bench/apply/fire.mlir`, Rows ids, sequential;
- `compiled-rows-par`: the same with the filling loop parallel (forced
  with `parallel-min-entities=1`);
- `compiled-gen`: `fire_generational.mlir`, the same program with a
  despawn that never runs, so ids are generational; sequential.

us per frame, median of 5 processes (spread), ns per gun in brackets
(1e6 rows from the second full run; the first agreed within 3.5%).

| guns | ships | c-index | c-atomic-par | c-buffered | compiled-rows | compiled-rows-par | compiled-gen |
|---|---|---|---|---|---|---|---|
| 1e5 | 16 | **49.3 (4%) [0.49]** | 6,694.3 (2%) [66.94] | 104.1 (5%) [1.04] | 91.8 (2%) [0.92] | 152.1 (3%) [1.52] | 103.2 (1%) [1.03] |
| 1e5 | 1e4 | **28.5 (2%) [0.28]** | 503.9 (2%) [5.04] | 83.6 (2%) [0.84] | 74.6 (1%) [0.75] | 137.8 (0%) [1.38] | 95.4 (2%) [0.95] |
| 1e5 | 1e6 | **85.6 (1%) [0.86]** | 437.5 (4%) [4.37] | 143.0 (2%) [1.43] | 128.2 (2%) [1.28] | 196.5 (2%) [1.97] | 220.6 (1%) [2.21] |
| 1e6 | 16 | **502.1 (1%) [0.50]** | 82,186.1 (3%) [82.19] | 546.7 (3%) [0.55] | 929.6 (1%) [0.93] | 659.0 (2%) [0.66] | 1,046.5 (0%) [1.05] |
| 1e6 | 1e4 | **288.2 (0%) [0.29]** | 4,695.7 (2%) [4.70] | 354.6 (3%) [0.35] | 754.5 (0%) [0.75] | 496.7 (1%) [0.50] | 978.1 (3%) [0.98] |
| 1e6 | 1e6 | **862.7 (1%) [0.86]** | 3,428.9 (8%) [3.43] | 898.1 (1%) [0.90] | 1,303.0 (1%) [1.30] | 1,040.2 (1%) [1.04] | 2,230.4 (9%) [2.23] |

The 1e5 rows are from a separate run with 500 frames instead of 50 (the
50-frame runs had spreads up to 65% there; their medians agree within 7%,
except compiled-rows-par at 1e4 ships: 122-138 us across the three runs).

### What holds

- Determinism is not what costs: atomics are far slower than the buffered,
  fixed-order form. At 1e6 guns `c-atomic-par` takes 3.3x the time of
  `compiled-rows-par` with targets spread over 1e6 ships, 9.4x over 1e4
  ships and 125x on 16 hot ships; it also loses to sequential `c-index`
  everywhere (4-164x). A parallel apply with atomics is not worth building.
- At 1e6 guns, parallel filling plus the sequential combine
  (`compiled-rows-par`) costs 1.21x (1e6 ships), 1.32x (16) and 1.72x (1e4)
  the time of sequential hand-written C with row indices. Against
  `c-buffered`, which does the same without ids, the compiled form takes
  16-40% longer; the difference is what the compiled combine does in
  addition (sentinel check, id decode, a branch on the archetype, the row
  checked against the count), not measured step by step.
- Sequential compiled applies cost 1.5-2.6x `c-index` at 1e6 guns (0.75 to
  1.30 ns per gun).
- Generational ids cost 1.7x Rows ids with targets spread over 1e6 ships
  (2.23 against 1.30 ns per gun), 1.3x over 1e4 ships and 1.1x on 16.
- At 1e5 guns a parallel filling loop loses to the sequential one (1.97
  against 1.28 ns per gun at 1e6 ships, 60-68 us more per frame), as for
  other parallel entity loops at this size; the default
  `parallel-min-entities` (1e6, checked against the count at run time)
  keeps such loops sequential.

### Measured, not explained

- Why the atomic loop is so slow with targets spread over 1e6 ships (3.4
  ns per gun, where contention should be rare): not investigated.
- `c-index` is slower on 16 ships than on 1e4 (0.50 against 0.29 ns per
  gun), and so are the compiled variants; a dependence through repeatedly
  updated `hp` values would fit, but it was not tested.
- Why generational ids cost most with 1e6 targets: the entity table's
  random reads (generation and location) are the obvious candidate, not
  verified.

## 2026-09-30: closing the gap between ecs.apply and hand-written C

Same machine and toolchain. Starting point: at 1e6 guns the compiled apply
with a parallel filling loop took 16-40% longer than `c-buffered`, the
same scheme written in C without ids. Measured step by step before
changing anything.

### Where the time went

Removing the combine loop's work one step at a time from the lowered IR
(E1-E4 cumulative, E5 alternative; us per frame at 1e6 guns, median of 5
processes, run twice; the runs agree within 3.5%):

| ships | c-index | c-buffered | E0 as generated | E1 counts loaded once | E2 -sentinel | E3 -archetype check | E4 -row check | E5 E1, one branch |
|---|---|---|---|---|---|---|---|---|
| 16 | 477 | 547 | 640 | 594 | 635 | 665 | 568 | 633 |
| 1e4 | 280 | 350 | 480 | 425 | 475 | 427 | 387 | 476 |
| 1e6 | 798 | 892 | 1,004 | 896 | 897 | 900 | 900 | 893 |

- Loading the count that Rows ids are checked against once, before the
  combine loop, saves 8-11%. The loop reloaded it for every row: it lives
  in the arena with the field being combined into, and the lowered code
  gives LLVM no way to tell them apart. Legal, since combining cannot
  change counts; with 1e6 targets it closes the gap entirely.
- The checks themselves are not the cost. Dropping the sentinel check made
  the loop 6-12% *slower* at 16 and 1e4 targets, and so did folding all
  three checks into one branch (E5); even with every check gone (E4) the
  loop is 4-10% slower than `c-buffered`. Not explained; it points at
  code layout or branch structure rather than the work, but that was not
  examined.
- The filling loop alone, timed without the combine loop, took 93-98 us per
  frame compiled against 52 us in C, whatever the number of ships: most of
  the remaining gap at 16 and 1e4 targets. On one thread both take about
  172 us; from 8 threads on the compiled one gets slower again (62 us at
  8, 74 at 12, 90 at 16) while C levels off at 48 us. The difference is
  about the same at 1e3 guns as at 1e6 (45 and 48 us), so it is a cost
  per parallel region, not per entity. It is not vectorisation (both loops are 8 wide)
  and hardly layout (C with its arrays at the arena's offsets: 55-78 us).
- It is a barrier. `--convert-scf-to-openmp` emits a work-sharing loop that
  ends with `__kmpc_barrier`, alone in a parallel region that ends with a
  barrier of its own; clang's `parallel for` makes no such call. Marking
  the loop `nowait` brings the compiled filling loop to 45 us at 1e3 guns
  and 62-63 us at 1e6 (C: 39-45 and 60). This also resolves the 2026-09-28
  entry that forks cost about 65 us against 35 us for an empty C region.

### After: counts and slots in use loaded once, `--ecs-omp-nowait`

The combine loop now loads the counts (Rows ids) or the slots in use
(slot ids) before it runs; `--ecs-omp-nowait` drops the barrier of a
work-sharing loop or sections construct that ends its parallel region,
and `parallel-stages` emits its sections without one. us per frame, median
of 5 processes (spread), ns per gun in brackets. The 1e6 rows are from a
full run, the 1e5 rows from a run with 500 frames. Load average 10 before
(left over from the previous run), 13-17 during, with no other process
above half a core; a first attempt was
discarded because XProtect used a full core and Discord was active.

| guns | ships | c-index | c-atomic-par | c-buffered | compiled-rows | compiled-rows-par | compiled-gen |
|---|---|---|---|---|---|---|---|
| 1e5 | 16 | **50.6 (1%) [0.51]** | 6,625.8 (1%) [66.26] | 104.5 (2%) [1.05] | 88.7 (1%) [0.89] | 103.2 (2%) [1.03] | 96.6 (2%) [0.97] |
| 1e5 | 1e4 | **28.9 (1%) [0.29]** | 512.9 (1%) [5.13] | 83.6 (2%) [0.84] | 68.9 (1%) [0.69] | 85.8 (4%) [0.86] | 89.8 (3%) [0.90] |
| 1e5 | 1e6 | **87.6 (1%) [0.88]** | 433.4 (3%) [4.33] | 142.8 (5%) [1.43] | 119.2 (1%) [1.19] | 141.8 (2%) [1.42] | 215.7 (1%) [2.16] |
| 1e6 | 16 | **503.8 (4%) [0.50]** | 79,105.2 (2%) [79.11] | 539.9 (4%) [0.54] | 884.9 (3%) [0.88] | 573.7 (4%) [0.57] | 970.9 (1%) [0.97] |
| 1e6 | 1e4 | **290.1 (2%) [0.29]** | 4,797.6 (5%) [4.80] | 355.3 (5%) [0.36] | 692.0 (3%) [0.69] | 391.9 (4%) [0.39] | 913.4 (2%) [0.91] |
| 1e6 | 1e6 | **863.0 (4%) [0.86]** | 3,412.9 (3%) [3.41] | 892.9 (3%) [0.89] | 1,196.0 (4%) [1.20] | 890.1 (5%) [0.89] | 2,181.8 (4%) [2.18] |

- `c-index` and `c-buffered` are within 1.5% of the previous run, so the
  comparison holds (`c-atomic-par` moved by up to 4%).
- At 1e6 guns the gap to `c-buffered` shrinks from +21% to +6% (16 ships),
  from +40% to +10% (1e4) and from +16% to none (1e6). Against sequential
  hand-written C with row indices the parallel apply now takes 1.03-1.35x.
- At 1e5 guns the parallel apply is 28-38% faster than before and within
  3% of `c-buffered`; it still loses to the sequential compiled apply
  there.
- Sequential combining gains 5-8% (Rows ids) and generational ids 2-7%.
- What remains at 16 and 1e4 targets (6-10%) is not explained; removing
  the checks did not remove it (see above).

### The barrier on the main benchmark

The fix applies to every parallel entity loop. A/B on the main benchmark
with parallel loops forced (`parallel-min-entities=1`), both builds in the
same rounds; us per frame, median of 5 processes. `stages-omp` is built
twice as a control (its sections are `nowait` either way now); the two
agree within 0.5%. All checksums match.

| variant | 1e3 | 1e4 | 1e5 | 1e6 | 1e7 |
|---|---|---|---|---|---|
| fused (sequential) | 0.29 | 4.5 | 45.9 | 454 | 4,546 |
| fused-entities-omp, with barrier | 124 | 125 | 138 | 201 | 1,839 |
| fused-entities-omp, nowait | 62 | 65 | 76 | 143 | 1,780 |
| entities-omp, with barrier | 308 | 311 | 327 | 398 | 2,242 |
| entities-omp, nowait | 156 | 160 | 175 | 246 | 2,101 |

- The fixed cost of a parallel frame halves: about 31 us per parallel
  region instead of 62 with 12 threads, which is what an empty C parallel
  region costs. The fused parallel frame at 1e6 is 29% faster (201 to 143
  us); at 1e7 3-6%.
- `stages-omp` at 1e3 takes 32 us against 63 us on 2026-09-29, consistent
  with its sections losing their barrier too; that compares runs on
  different days and is not an A/B.
- The crossover moves: forced parallel against sequential, the fused frame
  pays off between 1.5e5 (68 against 78 us) and 2e5 (91 against 83 us),
  the unfused one between 3e5 (153 against 192 us) and 5e5 (255 against
  210 us). The default `parallel-min-entities` (1e6) was set from the
  crossover with the barrier and is now conservative; not changed yet.

## 2026-09-30: change detection (reactive queries)

Same machine and toolchain, commit 0fc583a. `bench/reactive/run.py`: 1e6
units; each frame `hit` lowers the hp of the units a hash picks (the
fraction below), and `redraw` recomputes the bar of the units whose hp
changed. The bar depends on hp only, so every variant ends with the same
widths (checksums agree in all 10 configurations); the redraws per frame
show how many units a variant redrew. Light work is one multiply-add per
redraw, heavy a 32-step loop. us per frame, median of 5 processes
(spread), redraws per frame in brackets.

Variants: `c-poll` (no tracking, redraw everything), `c-rowstamp` (a tick
per changed row, redraw scans; the compiler's scheme by hand),
`c-blockstamp` (plus the newest tick per 64 rows; redraw skips old
blocks), `c-collector` (Entitas: a de-duplicated list of changed units),
`c-bevy` (row ticks for every mutable write of every component, observed
or not), `c-unity` (a version per 128-unit chunk, bumped for write access),
`compiled` (the reactive query, sequential) and `compiled-par` (the same
with parallel entity loops; the C variants are all sequential).

A first run was discarded: a Bazel build (about 7 cores) started with it,
and spreads reached 426%. The kept run: load average 2.4-3.2 (left from
the build), falling; nothing above 0.3 cores before, between and after.

### Light redraw

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-par |
|---|---|---|---|---|---|---|---|---|
| 0.01% | 443.9 (5%) [1,000,000] | 477.7 (3%) [107] | 263.8 (4%) [107] | 248.0 (6%) [107] | 482.5 (2%) [107] | 445.4 (6%) [1,000,000] | 447.2 (2%) [107] | **144.9 (4%) [107]** |
| 0.10% | 445.8 (4%) [1,000,000] | 496.5 (4%) [1,008] | 308.2 (3%) [1,008] | 257.6 (4%) [1,008] | 495.9 (7%) [1,008] | 453.3 (4%) [1,000,000] | 451.1 (3%) [1,008] | **143.8 (3%) [1,008]** |
| 1.00% | 482.3 (3%) [1,000,000] | 541.6 (4%) [9,995] | 562.4 (5%) [9,995] | 310.9 (3%) [9,995] | 539.5 (4%) [9,995] | 483.9 (2%) [1,000,000] | 483.2 (4%) [9,995] | **152.9 (5%) [9,995]** |
| 10.00% | 509.3 (4%) [1,000,000] | 610.3 (4%) [100,006] | 800.0 (1%) [100,006] | 437.0 (4%) [100,006] | 605.2 (3%) [100,006] | 511.4 (4%) [1,000,000] | 523.6 (3%) [100,006] | **158.2 (3%) [100,006]** |
| 100.00% | 441.2 (5%) [1,000,000] | 710.8 (1%) [1,000,000] | 909.5 (2%) [1,000,000] | 999.4 (2%) [1,000,000] | 849.5 (3%) [1,000,000] | 467.4 (2%) [1,000,000] | 573.2 (1%) [1,000,000] | **226.9 (4%) [1,000,000]** |

### Heavy redraw

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-par |
|---|---|---|---|---|---|---|---|---|
| 0.01% | 1,875.1 (2%) [1,000,000] | 495.7 (3%) [107] | 266.6 (1%) [107] | 252.1 (1%) [107] | 498.4 (2%) [107] | 1,866.2 (1%) [1,000,000] | 505.0 (3%) [107] | **153.8 (2%) [107]** |
| 0.10% | 1,876.7 (1%) [1,000,000] | 527.1 (3%) [1,008] | 334.7 (0%) [1,008] | 264.0 (4%) [1,008] | 535.2 (7%) [1,008] | 1,868.9 (3%) [1,000,000] | 528.6 (1%) [1,008] | **156.6 (0%) [1,008]** |
| 1.00% | 1,888.6 (1%) [1,000,000] | 679.4 (1%) [9,995] | 830.4 (3%) [9,995] | 366.0 (1%) [9,995] | 718.8 (2%) [9,995] | 1,898.2 (1%) [1,000,000] | 689.9 (2%) [9,995] | **209.6 (2%) [9,995]** |
| 10.00% | 1,918.2 (2%) [1,000,000] | 993.6 (2%) [100,006] | 1,203.7 (3%) [100,006] | 986.3 (2%) [100,006] | 1,026.6 (2%) [100,006] | 1,937.8 (1%) [1,000,000] | 1,021.8 (1%) [100,006] | **243.5 (3%) [100,006]** |
| 100.00% | 1,891.8 (1%) [1,000,000] | 6,661.2 (1%) [1,000,000] | 6,785.2 (1%) [1,000,000] | 6,882.6 (1%) [1,000,000] | 6,660.9 (1%) [1,000,000] | 1,885.9 (1%) [1,000,000] | 6,661.1 (0%) [1,000,000] | **1,022.6 (1%) [1,000,000]** |

### What holds

- No sequential scheme wins everywhere. With few changes the collector
  (Entitas) is fastest, 248-311 us light up to 1% changed, since its work
  follows the changes; block stamps come close up to 0.1% (264-308 us) and
  fall behind from 1% on, where about half the 64-unit blocks hold a
  change. Row stamps scan every unit, so they never cost less than a scan
  of the whole table (478-711 us light, polling 441-509). With every unit changed, polling wins: 441 us
  light and 1.89 ms heavy, against 711-999 us and 6.66-6.88 ms for the
  tracking schemes.
- Why polling wins heavy work 3.5x at 100%: its loop is vectorised and the
  check-first loops are not. Checked in the assembly: `c-poll` has 64
  vector floating-point ops with heavy work (4 light), `c-rowstamp` none;
  its per-unit `if` keeps the loop scalar.
- The compiled reactive query beats the same scheme written in C on light
  work, by 6% with few changes (447 against 478 us) and 19% with all
  changed (573 against 711 us): its masked, branch-free form vectorises
  (the assembly compares stamps four at a time, blends with `bsl`, and has
  vector `fmul`/`fadd`), while C's `if` does not. With heavy work the
  compiled body holds a loop, which the lowering does not run for rows the
  mask excludes, so it branches like C and matches it exactly (6.66 ms).
- Tracking by write access (Unity-style) degenerates to polling plus
  version bookkeeping here, since the writer has write access to every
  chunk: it redraws all 1e6 units at every rate, within -0.5% to +6% of
  polling. Tracking every component (Bevy-style) costs up to 2% over row
  stamps up to 0.1% changed, 3-6% at 1-10% with heavy work, and 20% with
  light work and all changed (850 against 711 us), from stamping the bar
  that no query observes.
- The parallel compiled query is 2.5-3.3x (light) and 3.3-6.5x (heavy)
  faster than the sequential one; no C variant runs in parallel, so this
  is not a comparison of schemes.

### Not measured

- A masked form of the heavy body (vectorised, but doing the work for
  every unit) would cost about what polling does: better from some change
  rate on, worse below; the lowering decides by the body's ops, not by a
  rate it cannot know.
- Combining schemes (a collector that falls back to polling past a size,
  or block stamps with per-block collectors) was not tried.

## 2026-10-01: event logs for reactive queries

Same machine and toolchain. Reactive queries now walk an event log of the
entities with events (default: an eighth of the entities, 131,072 entries
in 64 segments here) and scan only on their first run or when a segment
overflowed; `compiled` and `compiled-par` use it, `-scan` variants are the
same programs with `log 0` (the previous behaviour). Same benchmark,
1e6 units, us per frame, median of 5 processes (spread), redraws per frame
in brackets. Checksums agree in all configurations; nothing above 0.4
cores before, between or after (load average 2.4-2.5).

### Light redraw

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-scan | compiled-par | compiled-par-scan |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.01% | 446.7 (4%) [1,000,000] | 470.3 (4%) [107] | 262.7 (6%) [107] | 247.0 (7%) [107] | 467.8 (4%) [107] | 438.0 (3%) [1,000,000] | 273.8 (5%) [107] | 435.6 (2%) [107] | **80.5 (22%) [107]** | 143.1 (12%) [107] |
| 0.10% | 450.2 (5%) [1,000,000] | 485.7 (2%) [1,008] | 309.3 (3%) [1,008] | 257.1 (5%) [1,008] | 481.5 (2%) [1,008] | 441.9 (3%) [1,000,000] | 297.8 (4%) [1,008] | 447.6 (2%) [1,008] | **91.9 (2%) [1,008]** | 143.3 (3%) [1,008] |
| 1.00% | 472.3 (4%) [1,000,000] | 527.6 (2%) [9,995] | 553.6 (4%) [9,995] | 309.4 (4%) [9,995] | 537.0 (2%) [9,995] | 471.8 (2%) [1,000,000] | 373.4 (1%) [9,995] | 476.5 (2%) [9,995] | **147.2 (2%) [9,995]** | 151.9 (2%) [9,995] |
| 10.00% | 501.7 (3%) [1,000,000] | 597.7 (5%) [100,006] | 784.6 (4%) [100,006] | 434.3 (2%) [100,006] | 596.7 (3%) [100,006] | 499.0 (4%) [1,000,000] | 554.0 (2%) [100,006] | 517.3 (1%) [100,006] | 276.6 (1%) [100,006] | **157.5 (4%) [100,006]** |
| 100.00% | 441.0 (1%) [1,000,000] | 711.2 (1%) [1,000,000] | 908.9 (1%) [1,000,000] | 996.3 (0%) [1,000,000] | 842.1 (1%) [1,000,000] | 458.0 (3%) [1,000,000] | 1,068.6 (2%) [1,000,000] | 573.5 (2%) [1,000,000] | **215.9 (4%) [1,000,000]** | 222.9 (2%) [1,000,000] |

### Heavy redraw

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-scan | compiled-par | compiled-par-scan |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.01% | 1,853.4 (3%) [1,000,000] | 495.0 (2%) [107] | 266.7 (6%) [107] | 252.3 (4%) [107] | 493.9 (3%) [107] | 1,850.7 (2%) [1,000,000] | 272.7 (2%) [107] | 495.9 (3%) [107] | **82.6 (21%) [107]** | 150.3 (12%) [107] |
| 0.10% | 1,856.5 (2%) [1,000,000] | 525.0 (3%) [1,008] | 335.8 (3%) [1,008] | 264.4 (1%) [1,008] | 536.1 (3%) [1,008] | 1,874.3 (3%) [1,000,000] | 301.5 (2%) [1,008] | 534.9 (2%) [1,008] | **105.0 (12%) [1,008]** | 155.9 (3%) [1,008] |
| 1.00% | 1,898.0 (2%) [1,000,000] | 664.8 (1%) [9,995] | 837.7 (1%) [9,995] | 365.9 (2%) [9,995] | 713.2 (1%) [9,995] | 1,879.4 (3%) [1,000,000] | 444.9 (2%) [9,995] | 688.9 (2%) [9,995] | 214.1 (3%) [9,995] | **212.5 (1%) [9,995]** |
| 10.00% | 1,933.5 (2%) [1,000,000] | 997.3 (2%) [100,006] | 1,201.6 (2%) [100,006] | 981.8 (2%) [100,006] | 1,031.2 (3%) [100,006] | 1,921.2 (2%) [1,000,000] | 1,076.0 (3%) [100,006] | 1,016.9 (2%) [100,006] | 791.8 (0%) [100,006] | **241.9 (2%) [100,006]** |
| 100.00% | 1,892.7 (2%) [1,000,000] | 6,659.3 (1%) [1,000,000] | 6,782.9 (2%) [1,000,000] | 6,872.7 (1%) [1,000,000] | 6,656.7 (1%) [1,000,000] | 1,890.0 (2%) [1,000,000] | 7,157.2 (1%) [1,000,000] | 6,650.3 (1%) [1,000,000] | 1,033.5 (2%) [1,000,000] | **1,020.0 (2%) [1,000,000]** |

### How it got here

- A first version gave all threads of a parallel loop one shared count
  per log. Against thread count (10% changed, light): 585 us on 1
  thread, 2,956 on 2, 6,260 on 16, while the parallel scan went from 689
  to 152 us; so the shared count's contention it was. A per-thread log
  keyed by `omp_get_thread_num` was abandoned: a call in the parallel body
  keeps `--canonicalize` from inlining the `memref.alloca_scope` that
  `--convert-scf-to-openmp` wraps the body in, and `--convert-scf-to-cf`
  then rejects it. Segments by row range replaced both.
- The same run found a bug: writers that stop appending once a log is
  full left its count exactly one log ahead of the slowest reader, which
  readers took for "no overflow", so a frame changing every unit redrew
  only 131,072 of them (and the checksum differed). A full log now records
  the overflow; `test/Integration/reactive_overflow.test` fails without
  the fix.

### What holds

- Sequentially, walking the log pays where changes are sparse: 37% faster
  than scanning with light work at 0.01% changed (274 against 436 us), 22%
  at 1%, and 45% / 35% with heavy work at 0.01% / 1%. It comes within
  11-21% of the hand-written collector (247-309 us light) up to 1%.
- It costs where changes are dense: +7% at 10% light and +86% at 100%
  light (1,069 against 574 us); +6% and +8% with heavy work. A frame that
  changes everything pays for checking each write's segment and appending
  until the overflow marks it, and scans anyway; not measured step by
  step.
- With parallel loops, the log helps up to 0.1% changed (81 against 143
  us light, 83 against 150 heavy), ties at 1% and 100%, and loses at 10%
  (277 against 158 us light, 792 against 242 heavy). There 100,006 entries
  still fit (about 1,560 per segment of 2,048), so the query walks them,
  and the walk is sequential while the scan runs on all threads.

### Not done

- Walking the segments in parallel (an entity's latest entry is in one
  segment only, so it would be safe), or scanning earlier in parallel
  builds.
- Bounding the writers' cost in dense frames further, e.g. by skipping the
  segment check after a writer loop saw its segment full.

## 2026-10-01: walking event-log segments in parallel

Same machine and toolchain. With parallel entity loops, a reactive query
now walks its logs' segments in parallel once at least
`parallel-min-events` entries are pending (default 16,384); `compiled-par`
uses the default, `compiled-par-walk1` walks in parallel however few
entries are pending. 1e6 units, us per frame, median of 5 processes
(spread). Checksums agree in all configurations. A `launchd` spike (84% of
a core) before the light runs and Spotlight indexing (80%) after the heavy
ones; spreads are mostly at most 6% (up to 19% in a few heavy cells), and
the variants this change does not touch agree with the previous run
within 1-3%.

| changed | work | compiled (sequential walk) | compiled-par | compiled-par-walk1 | compiled-par-scan |
|---|---|---|---|---|---|
| 0.01% | light | 272.0 (3%) | **81.9 (10%)** | 113.3 (14%) | 141.0 (5%) |
| 0.10% | light | 294.2 (1%) | **91.4 (1%)** | 121.7 (9%) | 139.1 (4%) |
| 1.00% | light | 371.8 (2%) | **145.3 (7%)** | 154.1 (6%) | 151.6 (6%) |
| 10.00% | light | 560.9 (3%) | 162.8 (2%) | 159.8 (4%) | **155.8 (2%)** |
| 100.00% | light | 1,059.3 (2%) | 214.2 (2%) | **213.4 (4%)** | 226.0 (4%) |
| 0.01% | heavy | 279.5 (3%) | **84.4 (13%)** | 125.9 (11%) | 150.0 (10%) |
| 0.10% | heavy | 300.4 (19%) | **102.8 (6%)** | 125.7 (7%) | 152.1 (5%) |
| 1.00% | heavy | 442.8 (16%) | 216.4 (5%) | **164.5 (5%)** | 212.0 (5%) |
| 10.00% | heavy | 1,074.1 (2%) | **225.2 (3%)** | 225.3 (3%) | 242.6 (3%) |
| 100.00% | heavy | 7,237.7 (1%) | 1,033.2 (2%) | 1,031.3 (2%) | **1,020.6 (3%)** |

The full tables (all variants) are in the run's output; the C variants
match the previous section's.

### What holds

- The parallel walk closes the gap at 10% changed: 163 us against 277
  before (light) and 225 against 792 (heavy), 4.5% slower than the
  parallel scan light and 7% faster heavy. With logs, parallel builds are
  now within 5% of scanning or faster at every rate measured, and 1.5-1.8x
  faster up to 0.1% changed.
- Walking in parallel with few entries pending costs its fork: 113-126 us
  against 82-103 us for a sequential walk up to 0.1% changed. At 1%
  (about 10,000 entries, below the default threshold) the sequential walk
  wins with light work (145 against 154 us) and the parallel one with
  heavy work (216 against 165 us): the break-even lies near 10,000 entries
  and depends on the work per entity, which a fixed threshold cannot see.
  The default stays at 16,384; a profile of a recorded run could set it.

## 2026-10-01: relations, a spiking network

`bench/snn/run.py` (defaults: 5 rounds, 100 steps after 50 warm-up steps,
which also sort the edges): leaky integrate-and-fire neurons with K random
synapses each, a relation in ent-lang (`examples/snn.ent` pushes spikes
along outgoing synapses with an apply per edge, `examples/snn_pull.ent`
gathers along incoming ones with a lookup per edge) against hand-written C
over compressed rows: `c-push` (rows by source, firing neurons add into
their targets), `c-pull` (rows by target, weights stored in target order,
every neuron sums), `c-pull-par` (the same with `omp parallel for`). `-par`
ent variants: parallel entity loops, `parallel-min-entities=1`. The bias
sets the firing rate (the `firing` column: neurons firing per step);
weights scale with 1/K. us per step, median of 5 processes (spread).
Checksums (a hash of every potential's bits) agree in all configurations:
all variants add a neuron's inputs in the order of the source neurons.
M4 Max, 16 cores; load average 2.0 at the start and 5.0 at the end (the
parallel variants run 16 threads), WindowServer and Discord in the
background (about 23% and 9% of a core).

| neurons x synapses | bias | firing | c-push | c-pull | c-pull-par | ent-push | ent-push-par | ent-pull | ent-pull-par |
|---|---|---|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **20.3 (109%)** | 462.4 (8%) | 100.9 (30%) | 32.9 (79%) | 117.4 (30%) | 2,934.2 (14%) | 450.8 (6%) |
| 1e+04 x 100 | 0.3 | 7.4% | **53.0 (19%)** | 461.5 (11%) | 106.2 (15%) | 83.2 (12%) | 166.1 (13%) | 2,957.6 (12%) | 440.6 (6%) |
| 1e+05 x 100 | 0.15 | 2.0% | **343.3 (49%)** | 5,623.2 (8%) | 738.0 (13%) | 593.4 (16%) | 447.9 (9%) | 52,078.3 (17%) | 9,755.4 (2%) |
| 1e+05 x 100 | 0.3 | 7.3% | 1,066.1 (9%) | 5,601.3 (5%) | **777.1 (7%)** | 1,756.7 (8%) | 1,178.8 (11%) | 52,621.7 (11%) | 9,676.9 (2%) |
| 1e+06 x 20 | 0.15 | 2.4% | 2,579.0 (8%) | 15,853.2 (3%) | 2,612.8 (2%) | 3,995.4 (7%) | **2,268.8 (6%)** | 127,210.0 (4%) | 20,210.7 (4%) |
| 1e+06 x 20 | 0.3 | 7.9% | 5,674.4 (4%) | 16,602.6 (4%) | **3,050.1 (5%)** | 9,399.9 (8%) | 6,378.0 (11%) | 127,603.2 (4%) | 20,028.5 (5%) |

### What holds

- Push: `ent-push` takes 1.55-1.73x the time of `c-push` at every size and
  rate (1e4: 33 against 20 us, a noisy cell with 79-109% spread; 1e6 at
  2.4% firing: 4.0 against 2.6 ms). The generated code makes two passes
  where C makes one: the edge loop fills a target and a value per edge, and
  the combine at the end of the query walks the rows that fired and their
  edges again to add them in.
- Parallel push pays from 1e5 neurons: `ent-push-par` takes 0.67-0.76x of
  sequential `ent-push` at 1e5 and 0.57-0.68x at 1e6, and at 1e6 with 2.4%
  firing it is the fastest variant measured (2.27 ms against 2.58 for
  `c-push` and 2.61 for `c-pull-par`). At 1e4 the fork costs more than the
  step (117 against 33 us). The combine is sequential by construction; how
  much of the parallel step it takes was not measured.
- Push scales with the firing rate (2.2-3.1x from 2% to 7.5%), pull
  does not: it visits every edge every step. At these rates sequential push
  beats sequential pull in C by 2.9-23x.
- Pull is slow in ent-lang: `ent-pull` takes 6.3-9.4x the time of `c-pull`
  (1e5 x 100: 52 against 5.6 ms) and `ent-pull-par` 4.1-13x that of
  `c-pull-par`.

### Not measured

Candidate causes of the pull gap, none checked yet: (1) the generated
loop reaches an incoming edge's weight and source through the index by
target (an indirect, scattered load), where `c-pull` stores the weights
and sources in target order; (2) the lookup of the source's `fired`
locates the id per edge (unpack, archetype and bound checks), and the
bound may be reloaded per edge as the archetype count was in the apply
combine before it was hoisted; (3) `n.input += ...` loads and stores the
neuron's input once per edge rather than keeping the sum in a register.
A C pull variant through the same index, and the generated IR's inner
loop, would separate them.

## 2026-10-02: where the generated pull loses its time

The previous section left `ent-pull` at 6.3-9.4x `c-pull` with three
unchecked candidate causes. The generated inner loop (read in its
assembly) does, per incoming edge: a sequential load of the edge's
position from the index by target; a scattered load of the source id and
of the weight at that position in the table sorted by source; the
lookup's id checks (the archetype count is hoisted by LLVM, so that
candidate was wrong: the checks are a few integer instructions and a
branch); and a load, add and store of the neuron's input. `c-pull`'s loop
keeps the sum in a register and reads sources and weights sequentially
(7 instructions against 13). Three diagnostic variants add the generated
code's behaviour to `c-pull` one by one: `c-pull-index` reads sources and
weights through the index by target, `c-pull-index-store` also stores the
input on every edge (a compiler barrier per edge keeps it from staying in
a register), `c-pull-index-store-locate` also checks every source id like
the lookup does. `bench/snn/run.py --bias 0.15 --variants c-pull,
c-pull-index,c-pull-index-store,c-pull-index-store-locate,ent-pull`; us per
step, median of 5 processes (spread), checksums agree. Load average 3.5 at
the start and 2.5 at the end, WindowServer at about 21% of a core.

| neurons x synapses | bias | firing | c-pull | c-pull-index | c-pull-index-store | c-pull-index-store-locate | ent-pull |
|---|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **460.2 (9%)** | 1,197.0 (7%) | 2,953.8 (10%) | 2,955.3 (9%) | 2,887.8 (9%) |
| 1e+05 x 100 | 0.15 | 2.0% | **5,512.3 (6%)** | 39,499.7 (7%) | 53,890.0 (24%) | 53,417.1 (8%) | 51,907.7 (6%) |
| 1e+06 x 20 | 0.15 | 2.4% | **15,739.1 (3%)** | 108,499.7 (4%) | 127,561.7 (4%) | 130,283.4 (8%) | 124,169.4 (6%) |

### What holds

- With all three, C lands within 2-5% of `ent-pull` at every size
  (2,955 against 2,888 us at 1e4 x 100; 53.4 against 51.9 ms at 1e5 x 100;
  130 against 124 ms at 1e6 x 20): the three explain the gap.
- Reading through the index by target is the largest cost where the table
  does not fit in the caches: 7.2x at 1e5 x 100 and 6.9x at 1e6 x 20
  (1e7 and 2e7 edges, 80-160 MB of ids and weights read at scattered
  positions); 2.6x at 1e4 x 100.
- Storing the input on every edge costs 2.5x on top at 1e4 x 100, where the
  rest stays in cache, and 1.2-1.4x at the larger sizes.
- The id checks cost nothing measurable (-1% to +2%, within the spread).

## 2026-10-02: pull after sorting by target and carrying the sum

Two changes from the diagnosis above. (1) A relation the program visits
only by incoming edges is stored sorted by target (sorting first by source,
then stably by target, so an entity's incoming edges keep the order by
source): an `in` loop then reads sources and weights in order, without the
index. (2) A field of the visited entity that an edge loop sets is loaded
once before the loop, carried through it (and the `scf.if`s in it) as a
value, and stored once after it if it was set; the language guarantees
nothing else in the loop reaches it, which LLVM cannot see through the
arena's views. The id checks stay. `bench/snn/run.py --bias 0.15
--variants c-pull,c-pull-par,ent-pull,ent-pull-par`; us per step, median of
5 processes (spread), checksums agree. Load average 4.1 at the start
(falling after XProtect had scanned the new binaries) and 2.5 at the end.

| neurons x synapses | bias | firing | c-pull | c-pull-par | ent-pull | ent-pull-par |
|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | 449.6 (7%) | **103.8 (52%)** | 569.9 (6%) | 154.5 (42%) |
| 1e+05 x 100 | 0.15 | 2.0% | 5,353.0 (9%) | **749.8 (8%)** | 5,837.4 (4%) | 833.9 (10%) |
| 1e+06 x 20 | 0.15 | 2.4% | 15,309.1 (5%) | 2,592.6 (5%) | 16,926.0 (4%) | **2,373.6 (7%)** |

### What holds

- `ent-pull` takes 1.09-1.27x the time of `c-pull` (5.8 against 5.4 ms at
  1e5 x 100), down from 6.3-9.4x; `ent-pull-par` 1.11x `c-pull-par` at
  1e5 x 100 and 0.92x at 1e6 x 20 (2.37 against 2.59 ms), the fastest pull
  measured there. At 1e4 x 100 the parallel cells spread 42-52%.
- The C variants match the previous runs within 2-4%.
- Not measured: what the remaining 9-27% sequential gap is; the id checks
  (a branch per edge) and the loop not being vectorised are candidates.

## 2026-10-03: the rest of the pull gap is the lookup's checks

After the previous section, ent-pull still took 1.09-1.27x the time of
c-pull. Its inner loop (read in its assembly) is c-pull's plus, per edge,
the lookup's checks of the source id (archetype bits and row bound: `and`,
`lsr`, `cmp`, `ccmp`), a branch around the rest, and the flag saying the
input was set: 14 instructions against 7. The earlier diagnosis found the
checks free, but then the loop waited on scattered loads and a store per
edge. `c-pull-locate` is c-pull with only those checks, the sum kept in a
local as the generated code carries it (with the checks, clang otherwise
stores it on every edge). `bench/snn/run.py --bias 0.15 --variants
c-pull,c-pull-locate,ent-pull`; us per step, median of 5 processes
(spread), checksums agree. Load average 1.9 at the start and 1.6 at the
end.

| neurons x synapses | bias | firing | c-pull | c-pull-locate | ent-pull |
|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **421.4 (15%)** | 494.2 (6%) | 544.5 (5%) |
| 1e+05 x 100 | 0.15 | 2.0% | **5,187.8 (6%)** | 5,861.6 (5%) | 5,704.6 (5%) |
| 1e+06 x 20 | 0.15 | 2.4% | **14,776.9 (5%)** | 16,428.2 (8%) | 16,643.5 (3%) |

### What holds

- The checks cost c-pull 11-17% (1.17x at 1e4 x 100, 1.13x at 1e5 x 100,
  1.11x at 1e6 x 20).
- At 1e5 and 1e6, ent-pull matches c-pull with the checks (0.97x and
  1.01x): they are the whole remaining gap there.
- At 1e4 x 100, where everything stays in the caches, ent-pull takes 1.10x
  `c-pull-locate` (545 against 494 us; c-pull's cell spreads 15%): the
  checks explain about 60% of the gap there. The rest is not measured; the
  per-edge flag and the generated branch layout are candidates.

## 2026-10-03: typed relation ends

`relation Synapse { weight: f32 } from Neuron to Neuron` in both examples:
the relation names the component its ends have, connecting checks it, and
since no system despawns neurons or removes `Neuron`, lookups and applies
of `Neuron` through an edge's other end skip the id checks (the archetype
holding `Neuron` is the only candidate, so the row is read from the id
directly). Full `bench/snn/run.py`; us per step, median of 5 processes
(spread), checksums agree. Load average 5.1 at the start (from the test
suite run just before; the script builds every binary before measuring)
and 1.9 at the end.

| neurons x synapses | bias | firing | c-push | c-pull | c-pull-par | ent-push | ent-push-par | ent-pull | ent-pull-par |
|---|---|---|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **19.4 (97%)** | 454.1 (9%) | 104.7 (31%) | 31.6 (64%) | 111.9 (24%) | 463.8 (7%) | 144.0 (14%) |
| 1e+04 x 100 | 0.3 | 7.4% | **51.2 (13%)** | 456.1 (11%) | 108.3 (4%) | 76.8 (11%) | 160.6 (5%) | 466.0 (6%) | 141.7 (1%) |
| 1e+05 x 100 | 0.15 | 2.0% | **315.0 (18%)** | 5,454.8 (4%) | 766.4 (10%) | 539.0 (5%) | 408.5 (2%) | 5,670.2 (4%) | 759.2 (6%) |
| 1e+05 x 100 | 0.3 | 7.3% | 1,005.1 (6%) | 5,481.8 (3%) | 782.2 (7%) | 1,679.6 (2%) | 1,081.3 (14%) | 5,731.1 (4%) | **760.0 (2%)** |
| 1e+06 x 20 | 0.15 | 2.4% | 2,482.9 (3%) | 15,605.7 (2%) | 2,634.7 (3%) | 3,702.5 (4%) | **2,142.0 (4%)** | 15,723.2 (2%) | 2,191.0 (4%) |
| 1e+06 x 20 | 0.3 | 7.9% | 5,352.3 (9%) | 16,012.8 (3%) | 3,050.1 (1%) | 8,767.7 (2%) | 5,338.7 (3%) | 16,173.3 (3%) | **2,217.9 (5%)** |

### What holds

- `ent-pull` takes 1.01-1.05x the time of `c-pull` (1.02x at 1e4 x 100,
  1.04-1.05x at 1e5 x 100, 1.01x at 1e6 x 20), down from 1.09-1.27x.
- `ent-push` is unchanged within the spread: 1.49-1.71x `c-push` (before:
  1.55-1.73x). Its cost is the second pass (the combine), not the checks.
- `ent-pull-par` takes 0.97-0.99x `c-pull-par` at 1e5 x 100 and 0.73-0.83x
  at 1e6 x 20, where it is the fastest variant; at 1e4 x 100 1.31-1.38x
  (spreads up to 31%). The comparison is not like for like: ent's `-par`
  variants also run the integrate loop in parallel, the C ones only the
  propagation. Not measured how much that accounts for.

## 2026-10-03: push without the second pass

`ent-push` took 1.49-1.71x the time of `c-push`. `c-push-buffer` is c-push
in the generated code's two passes (every row writes a flag; firing rows
copy their edges' targets and values into buffers; then every row's flag
is read and the buffered values are added in). `bench/snn/run.py
--variants c-push,c-push-buffer,ent-push`; us per step, median of 5
processes (spread), checksums agree; load average 2.8 at the start and
2.4 at the end:

| neurons x synapses | bias | firing | c-push | c-push-buffer | ent-push |
|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **18.1 (95%)** | 27.2 (112%) | 29.6 (28%) |
| 1e+04 x 100 | 0.3 | 7.4% | **49.0 (35%)** | 70.1 (14%) | 74.2 (8%) |
| 1e+05 x 100 | 0.15 | 2.0% | **320.8 (36%)** | 505.6 (11%) | 518.5 (7%) |
| 1e+05 x 100 | 0.3 | 7.3% | **993.0 (35%)** | 1,652.6 (12%) | 1,621.7 (13%) |
| 1e+06 x 20 | 0.15 | 2.4% | **2,469.7 (4%)** | 3,546.0 (3%) | 3,688.9 (4%) |
| 1e+06 x 20 | 0.3 | 7.9% | **5,296.1 (5%)** | 8,452.5 (5%) | 8,664.1 (7%) |

`ent-push` lands within 0.98-1.09x of `c-push-buffer` everywhere: the two
passes are the gap. A loop that never runs in parallel visits the entities
(and their edges) in the order the query's end combines applies in, so an
apply whose field nothing else in the query touches (no get, set or lookup
of it, no second apply to it, no add or remove of its component) is now
combined as the loop visits, without buffers; likewise an accumulate whose
resource field nothing else in the query reads or accumulates into (the
`Stats.spikes += 1` in `integrate` was buffered per neuron and summed in a
second pass over all of them). Loops that may run in parallel keep the
buffers. `--ent-lower-to-loops=direct-applies=0` keeps the old form
(`ent-push-buffered`). `bench/snn/run.py --variants c-push,
ent-push-buffered,ent-push,ent-push-par,c-pull,ent-pull`; checksums agree;
load average 2.4 at the start and 2.0 at the end:

| neurons x synapses | bias | firing | c-push | ent-push-buffered | ent-push | ent-push-par | c-pull | ent-pull |
|---|---|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **20.6 (95%)** | 31.8 (62%) | 21.5 (82%) | 123.3 (13%) | 436.5 (15%) | 451.9 (10%) |
| 1e+04 x 100 | 0.3 | 7.4% | **49.9 (25%)** | 75.0 (8%) | 51.8 (19%) | 152.4 (13%) | 438.9 (5%) | 449.7 (4%) |
| 1e+05 x 100 | 0.15 | 2.0% | **309.0 (5%)** | 527.5 (10%) | 327.2 (7%) | 412.1 (7%) | 5,363.8 (2%) | 5,600.7 (50621%) |
| 1e+05 x 100 | 0.3 | 7.3% | **996.4 (47%)** | 1,643.3 (23%) | 1,040.0 (63%) | 1,080.2 (26%) | 5,389.5 (5%) | 5,605.0 (4%) |
| 1e+06 x 20 | 0.15 | 2.4% | 2,424.3 (5%) | 3,635.7 (36%) | 3,175.3 (15%) | **2,152.2 (17%)** | 15,272.1 (2%) | 15,136.7 (1%) |
| 1e+06 x 20 | 0.3 | 7.9% | **5,274.4 (12%)** | 8,656.6 (11%) | 6,207.5 (8%) | 5,360.0 (24%) | 15,777.2 (3%) | 15,693.6 (7%) |

### What holds

- `ent-push` takes 1.04-1.06x the time of `c-push` at 1e4 and 1e5 (327
  against 309 us at 1e5 x 100, 2% firing), down from 1.49-1.71x;
  `ent-push-buffered` matches the earlier `ent-push` within 1-3%.
- At 1e6 x 20 it takes 1.18-1.31x (3.18 against 2.42 ms at 2.4% firing).
  The direct accumulate took 9% off there (3.47 ms with only direct
  applies, in a run before). Not measured what the rest is; at 1e6
  neurons and few spikes the per-neuron work dominates. Candidates: the
  `Spiked` presence byte `integrate` writes for every neuron, and c-push
  computing each neuron's edges as `i * K` instead of loading offsets.
- `ent-pull` now takes 0.99-1.04x `c-pull` (its `integrate` has the same
  accumulate).
- One `ent-pull` process at 1e5 x 100, 2% firing, was an outlier (the
  spread column shows 50621%); the median agrees with the other runs.

## 2026-10-03: closing the gaps: like-for-like C, and the integration loop

Two things were left: push at 1e6 (1.18-1.31x) and the parallel
comparison (the C `-par` variant ran only the propagation in parallel).

The C side first. `c-pull-par` now runs the integration in parallel too
(the spike count as an OpenMP reduction), as ent-lang's `-par` variants do.
`c-push` now reads each neuron's edge range from an offsets array, as any
compressed-rows code must; until now it computed it as `i * K` with K a
compile-time constant, which no program over a real relation can, and
clang unrolled the edge loop (`c-push-fixed` keeps that form). And
`c-integrate` / `ent-integrate` time the integration alone (the example's
schedule without `propagate`; same dynamics as each other, not as the
others).

The ent side. The integration loop was 3.9x C's at 1e6 (790 against 202
us): C's vectorises (4 wide, interleaved 4), the generated one did not.
Three things, found one at a time with LLVM's vectoriser remarks:
- The directly combined spike count loaded and stored a cell of the arena
  on every spike, which LLVM cannot tell from the columns. The entity loop
  now carries such a resource cell as a loop value and stores it once
  after (unconditionally: a flag saying whether it changed made LLVM
  specialise the loop on it, with early exits).
- Both branches of `if v >= threshold` store `v`, `fired` and the presence
  of `Spiked`, each in its own block. When both branches of an `scf.if`
  store to the same column at the same index (and do nothing else with
  memory), the lowering now yields the values and stores once after the
  `if`, which becomes a select.
- That vectorised it at width 2, not 4: storing a presence byte per neuron.
  Clang does not vectorise the C loop at all with such a store (checked
  with the same loop plus a `uint8_t` store per neuron). The example kept
  the same fact twice, `fired` and the tag `Spiked`; C keeps one. Both
  examples now keep it as a value, as the design notes had it, and push
  filters `where n.fired != 0.0`. Then the loop vectorises 4 wide,
  interleaved 4, like C's. Finding: an optional component set or cleared
  for every entity in a hot loop costs vector width.

`bench/snn/run.py --rounds 7 --bias 0.15 --variants
c-integrate,ent-integrate`, then `--rounds 7` with the main variants; us per
step, median of 7 processes (spread), checksums agree. Load average 2.0 at
the start and 6.8 at the end (the parallel variants run 16 threads; Slack
at 12% of a core afterwards):

| neurons x synapses | bias | firing | c-integrate | ent-integrate |
|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.3% | **1.9 (136%)** | 1.9 (146%) |
| 1e+05 x 100 | 0.15 | 2.4% | 19.6 (2%) | **19.3 (19%)** |
| 1e+06 x 20 | 0.15 | 2.4% | **205.1 (1%)** | 336.4 (18%) |

| neurons x synapses | bias | firing | c-push-fixed | c-push | c-pull | c-pull-par | ent-push | ent-push-par | ent-pull | ent-pull-par |
|---|---|---|---|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **15.1 (111%)** | 16.7 (97%) | 455.1 (7%) | 185.9 (30%) | 17.0 (83%) | 117.2 (24%) | 466.4 (3%) | 141.8 (17%) |
| 1e+04 x 100 | 0.3 | 7.4% | 44.8 (14%) | 46.9 (11%) | 454.5 (6%) | 187.6 (2%) | **43.1 (12%)** | 164.8 (10%) | 465.5 (7%) | 141.6 (5%) |
| 1e+05 x 100 | 0.15 | 2.0% | **328.6 (57%)** | 337.6 (30%) | 5,602.4 (6%) | 801.7 (13%) | 342.9 (39%) | 419.0 (11%) | 5,770.0 (5%) | 789.7 (9%) |
| 1e+05 x 100 | 0.3 | 7.3% | 1,141.3 (45%) | 1,164.5 (45%) | 5,564.0 (4%) | **795.6 (7%)** | 1,120.6 (16%) | 1,146.1 (8%) | 5,785.4 (6%) | 804.4 (8%) |
| 1e+06 x 20 | 0.15 | 2.4% | 2,141.6 (44%) | 3,009.9 (19%) | 15,626.6 (5%) | **2,059.9 (13%)** | 2,987.8 (26%) | 2,372.2 (17%) | 15,172.6 (3%) | 2,175.9 (7%) |
| 1e+06 x 20 | 0.3 | 7.9% | 4,837.4 (16%) | 5,943.8 (25%) | 15,521.5 (3%) | **2,062.1 (4%)** | 5,902.6 (14%) | 6,322.3 (21%) | 15,310.9 (2%) | 2,246.6 (8%) |

Integration alone at 1e6 again, 9 processes, after the load average fell
below 3 (2.7 at the start, 2.6 at the end):

| neurons x synapses | bias | firing | c-integrate | ent-integrate |
|---|---|---|---|---|
| 1e+06 x 20 | 0.15 | 2.4% | **204.1 (4%)** | 333.6 (24%) |

### What holds

- Push and pull match hand-written C at every size measured: `ent-push`
  takes 0.92-1.02x the time of `c-push`, `ent-pull` 0.97-1.04x `c-pull`.
  Push cells at 1e4 and 1e5 spread widely for C and ent alike (up to 111%
  and 57%); the 1e6 cells spread 14-44% in this run.
- `c-push-fixed` (the old c-push) takes 0.71-0.81x of `c-push` at 1e6 and
  about the same elsewhere: the fixed, compile-time degree was most of the
  push gap at 1e6 reported before.
- With the integration parallel in C too, `ent-pull-par` takes 0.75-0.76x
  `c-pull-par` at 1e4 (C forks twice per step, ent's -par variants as
  well, but C's step is slower there; not measured why), 0.99-1.01x at
  1e5 and 1.06-1.09x at 1e6.
- The integration alone matches C at 1e4 and 1e5 (1.0x, 0.98x), from 3.9x
  before. At 1e6 it takes 1.63x (334 against 204 us in the 9-process run;
  ent's processes spread 24%, C's 4%). Not explained: the loops have the
  same shape and vector width. Push and pull at 1e6, which include the
  integration, match C within the spread.

## 2026-10-03: every benchmark on Linux (Ryzen AI 9 HX 370)

The first run off the Mac, at commit d023ec1 plus a portable timer in the
benchmark hosts (`clock_gettime(CLOCK_MONOTONIC)` where
`clock_gettime_nsec_np` does not exist). AMD Ryzen AI 9 HX 370 (12 cores,
24 threads, 22 GB visible), Arch Linux, kernel 7.1.9, on mains with the
`performance` governor. LLVM, MLIR, clang and `libomp` 22.1.8 from
conda-forge (Arch packages no MLIR), `clang -O2` without `-march`, so the
x86 code is baseline SSE2 where the Mac's had all of NEON. Every script
with its defaults, one after the other; `bench/run.py` and
`bench/layout/run.py` with 12 threads as on the Mac, the parallel variants
of apply, reactive and snn with the runtime's default thread count (not
recorded; 16 on the Mac). Checksums agree in every configuration of every
benchmark. Load average at the start of each run, mostly left by the one
before: main 0.8, main with `--blocktime 200` 4.3, streams 6.0, frame 4.1,
churn 5.4, apply 1.9, reactive 16.5 (apply's parallel variants had just
finished), snn 3.2; 1.1 after the last. No other process was checked.

"Mac" below is the most recent entry above for the same benchmark.

### Main benchmark, OpenMP runtime defaults

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 369 (4%) | 3,682 (6%) | 38,933 (3%) | 996,548 (3%) | 13,004,324 (2%) |
| stages-omp | 2,292 (34%) | 8,690 (35%) | 69,626 (35%) | 1,027,016 (12%) | 12,629,999 (3%) |
| entities-omp | 381 (4%) | 3,716 (3%) | 39,286 (3%) | 349,472 (11%) | 11,069,759 (2%) |
| fused | 302 (2%) | 2,962 (3%) | 32,725 (8%) | 953,716 (2%) | 12,054,948 (1%) |
| fused-entities-omp | 303 (3%) | 2,973 (2%) | 33,257 (6%) | 393,201 (16%) | 10,635,437 (2%) |
| c-fused | 262 (4%) | 2,861 (10%) | 32,791 (7%) | 964,663 (5%) | 12,390,410 (4%) |
| c-fused-restrict | 268 (4%) | 2,866 (10%) | 31,430 (11%) | 961,032 (4%) | 12,384,010 (2%) |

With `--blocktime 200`:

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 371 (2%) | 3,664 (4%) | 38,124 (8%) | 968,189 (2%) | 12,824,603 (4%) |
| stages-omp | 2,248 (36%) | 8,873 (33%) | 69,791 (43%) | 1,011,294 (6%) | 12,666,198 (6%) |
| entities-omp | 378 (1%) | 3,840 (5%) | 38,705 (3%) | 331,439 (5%) | 10,820,392 (3%) |
| fused | 301 (1%) | 2,971 (5%) | 32,267 (5%) | 942,930 (3%) | 11,953,735 (8%) |
| fused-entities-omp | 302 (1%) | 2,955 (3%) | 32,419 (6%) | 369,629 (13%) | 10,421,764 (0%) |
| c-fused | 259 (10%) | 2,810 (5%) | 31,289 (4%) | 929,174 (5%) | 12,311,514 (4%) |
| c-fused-restrict | 259 (3%) | 2,850 (5%) | 31,535 (5%) | 948,160 (3%) | 12,341,060 (6%) |

### Layout: streams and the frame by hand

GB/s moved (read + write), median of 5 processes:

| streams K | 1 | 2 | 3 | 4 | 5 | 8 | 16 |
|---|---|---|---|---|---|---|---|
| 1 MB total, 1 thread | 155 | 235 | 217 | 196 | 208 | 167 | 150 |
| 1 MB total, 12 threads | 736 | 622 | 558 | 608 | 542 | 488 | 206 |
| 160 MB total, 1 thread | 62 | 64 | 53 | 54 | 57 | 51 | 18 |
| 160 MB total, 1 thread, staggered | 62 | 64 | 62 | 53 | 58 | 51 | 52 |
| 160 MB total, 12 threads | 78 | 66 | 65 | 64 | 63 | 62 | 29 |

ns per frame:

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| soa | 333 (25%) | 3,256 (1%) | 38,797 (5%) | 930,588 (3%) | 11,909,984 (2%) |
| soa-omp | 3,724 (14%) | 4,358 (4%) | 11,152 (15%) | 378,550 (8%) | 10,501,304 (1%) |
| soa-stagger | 304 (3%) | 3,277 (1%) | 36,125 (6%) | 935,910 (3%) | 11,929,663 (1%) |
| soa-stagger-omp | 3,684 (10%) | 4,549 (5%) | 9,532 (16%) | 364,714 (3%) | 10,491,614 (0%) |
| aos | 835 (0%) | 8,312 (2%) | 84,164 (3%) | 1,262,117 (8%) | 13,094,405 (2%) |
| aos-omp | 3,025 (5%) | 4,283 (2%) | 17,638 (3%) | 440,582 (7%) | 11,525,962 (1%) |
| aosoa8 | 272 (1%) | 2,999 (1%) | 34,471 (3%) | 1,078,899 (3%) | 12,119,768 (1%) |
| aosoa8-omp | 2,665 (12%) | 5,297 (7%) | 22,690 (7%) | 483,757 (2%) | 12,559,641 (4%) |
| aosoa16 | 269 (1%) | 2,799 (1%) | 35,025 (3%) | 980,884 (2%) | 11,279,695 (1%) |
| aosoa16-omp | 3,475 (9%) | 3,830 (4%) | 7,029 (9%) | 350,266 (1%) | 10,046,926 (2%) |

### Churn, n = 1e6: us per frame (spread), best in bold

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **200.8 (6%)** | 484.4 (2%) | 568.8 (22%) | 202.3 (10%) | 500.0 (5%) | 465.8 (6%) |
| 1% | 0.1% | 248.1 (7%) | 498.2 (3%) | 604.7 (10%) | **218.7 (4%)** | 497.4 (6%) | 469.0 (3%) |
| 1% | 1.0% | 599.9 (3%) | 519.6 (3%) | 801.4 (8%) | **295.5 (10%)** | 525.4 (3%) | 485.9 (6%) |
| 10% | 0.0% | **220.2 (9%)** | 486.6 (5%) | 993.0 (3%) | 257.4 (12%) | 501.0 (6%) | 466.4 (7%) |
| 10% | 0.1% | 278.8 (7%) | 499.1 (1%) | 998.3 (1%) | **277.7 (10%)** | 509.8 (5%) | 472.1 (4%) |
| 10% | 1.0% | 673.1 (2%) | 520.7 (4%) | 1,040.0 (4%) | **371.3 (8%)** | 527.2 (4%) | 479.3 (8%) |
| 10% | 10.0% | 3,406.0 (4%) | 610.1 (8%) | 1,144.7 (5%) | 717.3 (11%) | 592.0 (4%) | **542.4 (15%)** |
| 50% | 0.0% | **299.7 (10%)** | 486.0 (5%) | 3,148.7 (2%) | 428.9 (6%) | 505.4 (6%) | 464.9 (5%) |
| 50% | 0.1% | **356.1 (9%)** | 495.6 (7%) | 3,154.4 (1%) | 455.6 (3%) | 508.7 (5%) | 466.4 (4%) |
| 50% | 1.0% | 806.1 (4%) | 523.4 (3%) | 3,206.2 (1%) | 585.2 (3%) | 524.7 (11%) | **493.1 (8%)** |
| 50% | 10.0% | 4,448.9 (4%) | 597.3 (9%) | 3,294.2 (1%) | 1,049.2 (7%) | 589.0 (1%) | **550.7 (7%)** |
| 90% | 0.0% | **362.8 (6%)** | 488.1 (7%) | 1,177.8 (4%) | 586.5 (7%) | 509.1 (5%) | 470.2 (7%) |
| 90% | 0.1% | **428.4 (6%)** | 497.7 (2%) | 1,183.6 (1%) | 607.9 (1%) | 507.7 (4%) | 466.4 (13%) |
| 90% | 1.0% | 848.6 (4%) | 522.0 (3%) | 1,228.6 (3%) | 747.5 (2%) | 524.9 (2%) | **484.3 (7%)** |
| 90% | 10.0% | 3,936.6 (6%) | 600.8 (5%) | 1,306.8 (6%) | 1,537.8 (6%) | 586.7 (5%) | **538.3 (9%)** |

The n = 1e5 table is not given: 39 of its 90 cells spread 49-60% and
another 21 spread 33-43%, for every variant.

### Apply: us per frame (spread), ns per gun in brackets

50 frames at both sizes (the Mac's 1e5 rows were taken with 500).

| guns | ships | c-index | c-atomic-par | c-buffered | compiled-rows | compiled-rows-par | compiled-gen |
|---|---|---|---|---|---|---|---|
| 1e5 | 16 | **52.2 (13%) [0.52]** | 2,904.3 (1%) [29.04] | 97.6 (18%) [0.98] | 61.5 (33%) [0.61] | 126.4 (13%) [1.26] | 52.8 (49%) [0.53] |
| 1e5 | 1e4 | **28.4 (50%) [0.28]** | 240.0 (7%) [2.40] | 90.3 (17%) [0.90] | 40.9 (49%) [0.41] | 116.1 (29%) [1.16] | 59.1 (49%) [0.59] |
| 1e5 | 1e6 | **53.5 (67%) [0.53]** | 190.5 (22%) [1.91] | 107.7 (33%) [1.08] | 73.4 (48%) [0.73] | 167.9 (38%) [1.68] | 125.7 (10%) [1.26] |
| 1e6 | 16 | **350.3 (3%) [0.35]** | 29,065.4 (0%) [29.07] | 664.2 (10%) [0.66] | 417.8 (2%) [0.42] | 923.4 (6%) [0.92] | 524.2 (2%) [0.52] |
| 1e6 | 1e4 | **285.3 (3%) [0.29]** | 1,707.3 (6%) [1.71] | 611.6 (3%) [0.61] | 417.1 (2%) [0.42] | 865.2 (7%) [0.87] | 605.1 (1%) [0.61] |
| 1e6 | 1e6 | **544.8 (4%) [0.54]** | 1,335.0 (4%) [1.34] | 1,325.3 (8%) [1.33] | 610.2 (2%) [0.61] | 1,656.2 (11%) [1.66] | 1,515.3 (15%) [1.52] |

### Reactive queries, 1e6 units, light redraw

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-scan | compiled-par | compiled-par-walk1 | compiled-par-scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0.01% | 457.4 (3%) [1,000,000] | 619.3 (2%) [107] | 425.5 (9%) [107] | 414.1 (2%) [107] | 617.4 (2%) [107] | 456.8 (1%) [1,000,000] | 394.9 (2%) [107] | 964.5 (7%) [107] | **126.5 (42%) [107]** | 152.0 (21%) [107] | 265.3 (21%) [107] |
| 0.10% | 466.8 (2%) [1,000,000] | 658.9 (3%) [1,008] | 529.4 (4%) [1,008] | 434.9 (1%) [1,008] | 667.2 (2%) [1,008] | 469.4 (3%) [1,000,000] | 418.1 (4%) [1,008] | 994.3 (2%) [1,008] | **130.1 (10%) [1,008]** | 165.9 (19%) [1,008] | 259.6 (21%) [1,008] |
| 1.00% | 510.6 (3%) [1,000,000] | 897.8 (4%) [9,995] | 1,151.6 (4%) [9,995] | 543.5 (2%) [9,995] | 940.4 (10%) [9,995] | 514.3 (8%) [1,000,000] | 598.1 (6%) [9,995] | 1,053.7 (4%) [9,995] | 253.1 (5%) [9,995] | **199.5 (16%) [9,995]** | 268.8 (15%) [9,995] |
| 10.00% | 485.5 (4%) [1,000,000] | 775.2 (2%) [100,006] | 818.0 (13%) [100,006] | 594.1 (3%) [100,006] | 1,013.3 (5%) [100,006] | 467.1 (7%) [1,000,000] | 872.8 (6%) [100,006] | 867.9 (2%) [100,006] | 422.3 (28%) [100,006] | 449.5 (10%) [100,006] | **270.3 (20%) [100,006]** |
| 100.00% | 472.1 (5%) [1,000,000] | 894.3 (1%) [1,000,000] | 926.1 (2%) [1,000,000] | 1,005.8 (2%) [1,000,000] | 1,171.1 (2%) [1,000,000] | 483.2 (9%) [1,000,000] | 1,753.3 (1%) [1,000,000] | 907.7 (7%) [1,000,000] | 492.1 (6%) [1,000,000] | 490.9 (9%) [1,000,000] | **254.9 (12%) [1,000,000]** |

### Reactive queries, 1e6 units, heavy redraw

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-scan | compiled-par | compiled-par-walk1 | compiled-par-scan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0.01% | 3,562.3 (1%) [1,000,000] | 622.8 (2%) [107] | 428.2 (8%) [107] | 412.6 (6%) [107] | 618.6 (1%) [107] | 3,549.9 (3%) [1,000,000] | 393.4 (1%) [107] | 618.8 (6%) [107] | **127.8 (25%) [107]** | 158.7 (33%) [107] | 171.1 (66%) [107] |
| 0.10% | 3,558.9 (0%) [1,000,000] | 698.7 (3%) [1,008] | 558.8 (2%) [1,008] | 438.8 (12%) [1,008] | 693.5 (3%) [1,008] | 3,552.9 (1%) [1,000,000] | 423.0 (4%) [1,008] | 697.4 (1%) [1,008] | **154.7 (41%) [1,008]** | 161.4 (23%) [1,008] | 181.1 (14%) [1,008] |
| 1.00% | 3,608.2 (3%) [1,000,000] | 1,077.0 (6%) [9,995] | 1,374.7 (3%) [9,995] | 605.6 (9%) [9,995] | 1,110.8 (2%) [9,995] | 3,606.1 (3%) [1,000,000] | 658.7 (5%) [9,995] | 1,064.8 (2%) [9,995] | 415.8 (15%) [9,995] | **217.3 (33%) [9,995]** | 235.2 (16%) [9,995] |
| 10.00% | 3,576.9 (2%) [1,000,000] | 1,680.8 (1%) [100,006] | 1,699.5 (1%) [100,006] | 1,726.9 (2%) [100,006] | 1,720.9 (1%) [100,006] | 3,567.2 (1%) [1,000,000] | 1,796.1 (1%) [100,006] | 1,710.4 (1%) [100,006] | 554.7 (53%) [100,006] | 503.2 (28%) [100,006] | **356.1 (10%) [100,006]** |
| 100.00% | 3,575.3 (1%) [1,000,000] | 13,186.4 (1%) [1,000,000] | 13,251.4 (0%) [1,000,000] | 13,289.8 (1%) [1,000,000] | 13,200.0 (0%) [1,000,000] | 3,566.4 (1%) [1,000,000] | 13,979.5 (0%) [1,000,000] | 13,166.3 (2%) [1,000,000] | 1,493.3 (1%) [1,000,000] | 1,495.6 (0%) [1,000,000] | **1,361.1 (3%) [1,000,000]** |

### Spiking network: us per step (spread)

| neurons x synapses | bias | firing | c-push | c-pull | c-pull-par | ent-push | ent-push-par | ent-pull | ent-pull-par |
|---|---|---|---|---|---|---|---|---|---|
| 1e+04 x 100 | 0.15 | 2.0% | **27.7 (38%)** | 389.4 (3%) | 167.6 (35%) | 28.2 (39%) | 103.6 (23%) | 423.1 (13%) | 156.1 (25%) |
| 1e+04 x 100 | 0.3 | 7.4% | **40.0 (105%)** | 388.2 (5%) | 168.3 (23%) | 41.9 (83%) | 168.3 (15%) | 422.6 (5%) | 150.3 (66%) |
| 1e+05 x 100 | 0.15 | 2.0% | 473.6 (4%) | 4,665.3 (8%) | 1,188.2 (12%) | **470.8 (19%)** | 684.5 (43%) | 4,887.7 (4%) | 1,266.3 (16%) |
| 1e+05 x 100 | 0.3 | 7.3% | 1,452.6 (2%) | 4,701.2 (8%) | **1,169.9 (10%)** | 1,408.1 (2%) | 2,256.3 (54%) | 4,928.6 (4%) | 1,237.0 (18%) |
| 1e+06 x 20 | 0.15 | 2.4% | 5,950.3 (10%) | 17,945.7 (3%) | **3,409.0 (8%)** | 5,875.8 (1%) | 7,772.0 (17%) | 18,832.1 (4%) | 3,681.3 (9%) |
| 1e+06 x 20 | 0.3 | 7.9% | 11,632.9 (3%) | 17,751.6 (5%) | **3,417.2 (5%)** | 11,681.9 (4%) | 17,008.3 (50%) | 18,869.6 (6%) | 3,776.0 (4%) |

### What holds here as on the Mac

- Generated fused code matches hand-written C from 1e4 entities on (within
  3.5%; `restrict` changes nothing). At 1e3 it takes 15% longer (302
  against 262 ns), where the Mac showed no difference.
- Fusion pays more: 20% at 1e4, 16% at 1e5, 4% at 1e6, 7% at 1e7.
- `ent-push` takes 0.97-1.05x the time of `c-push` at every size and rate.
  `ent-pull` takes 1.05-1.09x `c-pull` (Mac: 0.97-1.04x), and
  `ent-pull-par` 0.89-0.93x `c-pull-par` at 1e4 and 1.06-1.11x from 1e5.
- AoS is the slowest layout on one thread (2.2-2.6x SoA up to 1e5, 1.1-1.4x
  beyond). Staggering turns the collapse at 16 streams in DRAM into a
  gradual decline (18 to 52 GB/s).
- No optional-component storage wins everywhere; branch-free beats the
  `if` (up to 6.5x at 50% density); atomics lose to the sequential
  compiled apply and to `c-index` everywhere; among the sequential
  reactive schemes polling wins when every unit changed, and none wins at
  every rate.

### What differs

- This machine is faster in the caches and slower out of them. The fused
  frame takes 0.66x the Mac's time at 1e4 and 0.72x at 1e5, but 2.1x at
  1e6 and 2.7x at 1e7 (12.1 against 4.54 ms). The streams say why: one
  thread moves 155-235 GB/s within 1 MB in up to 5 streams (Mac: 124-143)
  but 53-64 GB/s over 160 MB (Mac: 121-141), and 12 threads move 62-78
  GB/s there in up to 8 streams (Mac: 358-405). One core nearly saturates this machine's memory.
- So entity parallelism stops paying where the Mac's paid most: the fused
  parallel frame is 2.4x faster than fused at 1e6 (393 against 954 us) and
  1.13x at 1e7 (10.6 against 12.1 ms; Mac: 2.2x and 2.5x). Parallel pull,
  which reads more than it writes, still gains 5.0-5.3x at 1e6 neurons.
- Forks are cheap. `stages-omp` takes 2.3 us at 1e3 (Mac: 64 us with
  runtime defaults, 2.3-3.0 with `KMP_BLOCKTIME=200`), and `--blocktime
  200` changes nothing here (every cell within the spread): this `libomp`
  evidently keeps its workers spinning by default.
- Staggered columns no longer speed the frame up from 1e6 on (-0.6% and
  -0.2%; 7% at 1e5, where the Mac gained 7-8% at every size), consistent
  with DRAM being the limit on one thread already.
- Churn at 1e6: the compiled masked form no longer beats the hand-written
  select per system (500-509 against 484-488 us without churn; Mac 285 against
  313), and fused it leads by 4% (466 us), not 18%. A change costs
  archetype moves 30-45 ns (Mac: 11-13), yet their status pass is cheap
  enough that they win without churn at every density, 90% included,
  where the compiled fused form won on the Mac.
- Apply at 1e6 guns: the sequential compiled apply is closer to `c-index`
  (1.12-1.46x; Mac 1.39-2.39x), and the parallel filling loop loses to it
  everywhere (923 against 418 us on 16 ships; on the Mac it won by
  1.3-1.8x). `c-buffered` loses to `c-index` by 1.9-2.4x (Mac:
  1.03-1.22x), so it is the scheme, not the generated code;
  `compiled-rows-par` takes 1.25-1.41x `c-buffered` (Mac: 1.0-1.10x).
  `c-atomic-par` is 2.6-2.8x faster than on the Mac and still takes
  2.5-83x the time of `c-index`.
- Parallel push never pays: `ent-push-par` takes 1.3-4.0x the time of
  `ent-push` at every size (7.8 against 5.9 ms at 1e6, 2.4% firing, where
  it was the Mac's fastest variant earlier).
- Reactive, few changes: the compiled event log beats the hand-written
  collector up to 0.1% changed (395 against 414 us, 418 against 435;
  on the Mac the collector led by 11-21%).
- Reactive, scanning: `compiled-scan` with light work takes 1.56x the
  time of `c-rowstamp` at 0.01% changed and 1.17x at 1% (965 against 619
  us), where on the Mac it was 6-19% faster. With heavy work the two
  match, as before.
- Heavy redraw costs about twice the Mac's: `c-poll` 3.56 against 1.89
  ms, the check-first loops 13.2 against 6.66 ms with all changed.

### Measured, not explained

- Whether baseline SSE2 is why the masked forms lost their lead (churn's
  compiled select, the reactive scan): they depend on blends and wide
  compares, which NEON has and SSE2 lacks; no `-march=native` build was
  measured.
- Why the buffered apply costs twice `c-index` here.
- The spreads of 33-60% in churn at 1e5 and in many parallel cells. They
  look bimodal; this CPU has two kinds of cores (4 Zen 5, 8 Zen 5c) and
  no process was pinned, which would fit but was not checked.
- The reactive run started at load average 16.5; its sequential C cells
  spread at most 13%, its parallel ones up to 66%, and it was not repeated.
- The generated frame at 1e3 taking 15% longer than C.

## 2026-10-03: Linux again, pinned and with `-march=native`

Two questions the entry above left: whether the 33-60% spreads come from
this CPU's two kinds of cores, and whether baseline SSE2 is why the masked
forms lost their lead over C. Same machine, toolchain and commit; the
scripts now pass `BENCH_CFLAGS` to every compile. CPUs 0-3 are Zen 5 cores
(5.16 GHz, 16 MB L3 shared by the four), CPUs 4-11 Zen 5c (3.29 GHz, 8 MB
L3 shared by the eight), each with a second hardware thread. Sequential
runs under `taskset -c 2` (Zen 5) or `-c 8` (Zen 5c); parallel runs with
`OMP_NUM_THREADS=12 OMP_PLACES=cores OMP_PROC_BIND=close`, one thread per
core. "native" adds `BENCH_CFLAGS=-march=native` (clang takes it as
`znver5`; the binaries use 512-bit vectors and mask registers). Baseline
and native are separate runs, one after the other, not the same rounds.
Checksums agree in every configuration, between baseline and native too
where a script compares them. Load average 0.3-1.3 at the start of each
sequential run, 1.1-8.2 for the parallel ones (left by the one before).

### The spread was the two kinds of cores

Churn at n = 1e5 on one Zen 5 core, us per frame (spread), the table the
entry above could not give:

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **13.4 (4%)** | 33.9 (3%) | 33.5 (3%) | 13.6 (2%) | 30.6 (1%) | 27.9 (2%) |
| 1% | 0.1% | 14.6 (7%) | 33.7 (1%) | 36.0 (1%) | **13.8 (3%)** | 30.7 (3%) | 27.9 (2%) |
| 1% | 1.0% | 20.3 (7%) | 34.4 (2%) | 37.3 (2%) | **14.8 (2%)** | 31.4 (3%) | 28.6 (2%) |
| 10% | 0.0% | **14.0 (2%)** | 33.8 (2%) | 27.2 (4%) | 16.9 (2%) | 30.7 (2%) | 28.0 (2%) |
| 10% | 0.1% | **15.0 (5%)** | 33.8 (2%) | 48.7 (2%) | 17.1 (2%) | 30.8 (3%) | 28.2 (3%) |
| 10% | 1.0% | 21.1 (3%) | 34.3 (2%) | 84.7 (2%) | **18.1 (1%)** | 31.0 (2%) | 28.6 (2%) |
| 10% | 10.0% | 72.7 (4%) | 37.8 (2%) | 92.9 (2%) | **26.0 (3%)** | 34.9 (4%) | 32.0 (3%) |
| 50% | 0.0% | **16.8 (1%)** | 33.8 (2%) | 247.9 (1%) | 29.7 (1%) | 30.5 (2%) | 28.1 (2%) |
| 50% | 0.1% | **17.9 (5%)** | 33.8 (2%) | 259.3 (2%) | 29.9 (2%) | 30.7 (2%) | 28.0 (2%) |
| 50% | 1.0% | **24.9 (8%)** | 34.4 (2%) | 285.6 (1%) | 31.7 (1%) | 31.5 (1%) | 28.6 (2%) |
| 50% | 10.0% | 78.3 (3%) | 37.7 (1%) | 304.5 (1%) | 43.1 (4%) | 35.6 (3%) | **32.0 (1%)** |
| 90% | 0.0% | **19.7 (2%)** | 33.8 (2%) | 42.6 (2%) | 43.2 (2%) | 30.8 (3%) | 28.0 (2%) |
| 90% | 0.1% | **20.8 (7%)** | 33.7 (1%) | 72.9 (4%) | 43.3 (2%) | 30.7 (1%) | 28.1 (1%) |
| 90% | 1.0% | **26.9 (8%)** | 34.4 (2%) | 103.1 (1%) | 45.3 (2%) | 31.5 (3%) | 28.6 (3%) |
| 90% | 10.0% | 78.4 (6%) | 37.8 (2%) | 108.6 (2%) | 57.7 (3%) | 34.8 (3%) | **32.0 (4%)** |

Spreads are 1-8% where the unpinned run had 33-60% in 60 of 90 cells. On a
Zen 5c core the same run takes 1.5x as long in every cell that fits the
caches (51.0 against 33.8 us for wide-select, 46.5 against 30.7 compiled),
the ratio of the clocks (1.57), and 1.4-1.8x at 1e6; its spreads are 0-6%
at 1e5 and at most 11% at 1e6.
The unpinned medians were mixtures of the two.

The other pinned baselines reproduce the unpinned runs: the main
benchmark's sequential variants within 3% at every size, the reactive
benchmark's sequential variants within 6% (that run had started at load
average 16.5), churn at 1e6 within 3% for the forms without a branch.

### Main benchmark, one Zen 5 core, ns per frame

Baseline:

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 366 (1%) | 3,694 (4%) | 39,349 (5%) | 970,805 (2%) | 12,839,767 (1%) |
| fused | 299 (1%) | 2,922 (2%) | 32,844 (5%) | 921,698 (2%) | 11,983,479 (0%) |
| c-fused | 256 (2%) | 2,798 (3%) | 32,116 (15%) | 936,066 (2%) | 12,401,045 (1%) |
| c-fused-restrict | 254 (3%) | 2,835 (10%) | 32,486 (10%) | 929,980 (2%) | 12,450,925 (1%) |

Native:

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| loops | 152 (5%) | 2,029 (3%) | 33,304 (3%) | 853,093 (1%) | 12,410,493 (1%) |
| fused | 119 (7%) | 1,663 (2%) | 29,440 (6%) | 858,827 (1%) | 12,175,067 (0%) |
| c-fused | 120 (13%) | 1,692 (7%) | 29,193 (3%) | 861,147 (4%) | 12,364,062 (2%) |
| c-fused-restrict | 125 (7%) | 1,682 (7%) | 29,727 (2%) | 855,925 (3%) | 12,206,697 (1%) |

Twelve threads, one per core, baseline:

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| stages-omp | 1,508 (5%) | 4,304 (1%) | 34,528 (13%) | 817,753 (2%) | 11,734,384 (1%) |
| entities-omp | 379 (1%) | 3,792 (4%) | 39,741 (4%) | 326,074 (6%) | 10,801,466 (0%) |
| fused-entities-omp | 303 (1%) | 3,000 (2%) | 32,555 (14%) | 358,530 (11%) | 10,403,168 (0%) |

Native:

| variant | n=1e3 | n=1e4 | n=1e5 | n=1e6 | n=1e7 |
|---|---|---|---|---|---|
| stages-omp | 1,325 (8%) | 3,458 (3%) | 31,566 (5%) | 712,261 (3%) | 11,479,436 (1%) |
| entities-omp | 150 (5%) | 2,031 (5%) | 32,950 (2%) | 342,090 (2%) | 11,180,478 (1%) |
| fused-entities-omp | 122 (5%) | 1,702 (3%) | 28,452 (6%) | 376,893 (3%) | 10,545,950 (1%) |

### Churn, one Zen 5 core, native: us per frame (spread)

n = 1e5:

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **11.3 (5%)** | 17.9 (2%) | 17.4 (1%) | 11.5 (4%) | 17.5 (3%) | 15.4 (7%) |
| 1% | 0.1% | 12.3 (11%) | 17.7 (3%) | 17.2 (3%) | **11.6 (3%)** | 17.5 (2%) | 15.4 (1%) |
| 1% | 1.0% | 18.0 (5%) | 18.3 (2%) | 17.8 (2%) | **12.6 (2%)** | 17.9 (3%) | 15.5 (3%) |
| 10% | 0.0% | **11.6 (3%)** | 17.8 (3%) | 17.1 (2%) | 14.7 (2%) | 17.4 (0%) | 15.2 (4%) |
| 10% | 0.1% | **13.0 (7%)** | 17.9 (3%) | 17.3 (2%) | 14.9 (1%) | 17.5 (2%) | 15.4 (3%) |
| 10% | 1.0% | 19.2 (13%) | 18.4 (3%) | 17.7 (3%) | 16.3 (4%) | 18.1 (4%) | **15.6 (3%)** |
| 10% | 10.0% | 69.5 (4%) | 21.7 (3%) | 21.5 (1%) | 24.7 (3%) | 21.9 (5%) | **19.3 (2%)** |
| 50% | 0.0% | **13.9 (2%)** | 17.8 (2%) | 17.1 (2%) | 27.0 (1%) | 17.4 (3%) | 15.2 (3%) |
| 50% | 0.1% | **15.0 (11%)** | 17.7 (2%) | 17.2 (4%) | 27.3 (1%) | 17.4 (2%) | 15.2 (2%) |
| 50% | 1.0% | 22.0 (6%) | 18.1 (1%) | 17.8 (1%) | 29.5 (1%) | 18.1 (4%) | **15.8 (3%)** |
| 50% | 10.0% | 74.8 (3%) | 21.4 (6%) | 21.5 (2%) | 41.8 (2%) | 22.0 (5%) | **19.3 (7%)** |
| 90% | 0.0% | 16.5 (3%) | 18.0 (4%) | 17.1 (2%) | 39.5 (1%) | 17.5 (3%) | **15.3 (2%)** |
| 90% | 0.1% | 17.5 (4%) | 17.9 (2%) | 17.4 (2%) | 39.9 (2%) | 17.5 (4%) | **15.3 (3%)** |
| 90% | 1.0% | 24.2 (3%) | 18.4 (2%) | 17.9 (4%) | 42.2 (2%) | 18.0 (2%) | **15.6 (3%)** |
| 90% | 10.0% | 75.1 (4%) | 21.8 (7%) | 21.3 (4%) | 54.5 (5%) | 21.9 (5%) | **19.0 (4%)** |

n = 1e6:

| density | churn/frame | archetypes | wide-select | wide-branch | sparse-set | compiled | compiled-fused |
|---|---|---|---|---|---|---|---|
| 1% | 0.0% | **169.7 (7%)** | 369.7 (5%) | 455.3 (7%) | 188.3 (11%) | 382.8 (7%) | 405.1 (7%) |
| 1% | 0.1% | 215.0 (2%) | 377.1 (2%) | 467.0 (3%) | **192.9 (7%)** | 390.5 (2%) | 403.8 (2%) |
| 1% | 1.0% | 567.7 (3%) | 395.4 (6%) | 485.9 (5%) | **260.5 (8%)** | 410.6 (3%) | 417.5 (3%) |
| 10% | 0.0% | **181.8 (25%)** | 373.7 (10%) | 452.9 (15%) | 229.3 (10%) | 390.5 (9%) | 404.7 (14%) |
| 10% | 0.1% | **234.9 (13%)** | 374.6 (4%) | 463.4 (15%) | 236.8 (15%) | 393.4 (15%) | 410.0 (16%) |
| 10% | 1.0% | 634.6 (1%) | 395.3 (2%) | 483.6 (7%) | **332.2 (3%)** | 409.0 (1%) | 421.4 (5%) |
| 10% | 10.0% | 3,391.2 (2%) | 460.9 (2%) | 530.9 (14%) | 700.7 (15%) | **456.5 (10%)** | 503.6 (8%) |
| 50% | 0.0% | **259.5 (6%)** | 368.8 (4%) | 457.5 (10%) | 381.8 (6%) | 386.5 (3%) | 401.7 (2%) |
| 50% | 0.1% | **313.3 (3%)** | 377.2 (1%) | 463.7 (1%) | 405.0 (2%) | 392.0 (5%) | 406.7 (9%) |
| 50% | 1.0% | 758.1 (1%) | **395.3 (0%)** | 486.6 (2%) | 530.2 (1%) | 405.9 (3%) | 422.8 (5%) |
| 50% | 10.0% | 4,444.5 (1%) | 458.2 (3%) | 531.3 (2%) | 1,042.7 (3%) | **453.2 (1%)** | 507.3 (5%) |
| 90% | 0.0% | **315.3 (3%)** | 372.9 (2%) | 452.2 (1%) | 525.5 (2%) | 385.0 (1%) | 399.1 (6%) |
| 90% | 0.1% | **372.8 (2%)** | 376.6 (2%) | 460.8 (2%) | 555.1 (2%) | 390.3 (1%) | 403.7 (3%) |
| 90% | 1.0% | 805.8 (1%) | **392.9 (1%)** | 491.2 (5%) | 699.5 (2%) | 408.4 (4%) | 417.7 (4%) |
| 90% | 10.0% | 3,938.4 (1%) | 463.3 (3%) | 534.3 (9%) | 1,507.2 (2%) | **451.9 (4%)** | 502.2 (6%) |

### Reactive queries, one Zen 5 core, native: us per frame (spread)

Light redraw:

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-scan |
|---|---|---|---|---|---|---|---|---|
| 0.01% | **178.3 (2%)** | 625.4 (3%) | 270.6 (1%) | 251.4 (1%) | 1,072.6 (1%) | 186.6 (2%) | 279.4 (4%) | 525.6 (19%) |
| 0.10% | **178.0 (32%)** | 634.5 (7%) | 399.2 (2%) | 267.5 (4%) | 1,079.0 (6%) | 186.7 (4%) | 330.4 (5%) | 539.3 (18%) |
| 1.00% | **177.8 (2%)** | 627.7 (6%) | 849.9 (2%) | 355.8 (1%) | 1,082.7 (2%) | 185.9 (1%) | 398.1 (5%) | 539.6 (17%) |
| 10.00% | **178.8 (4%)** | 627.7 (6%) | 801.0 (2%) | 453.6 (3%) | 1,077.1 (3%) | 187.9 (3%) | 793.8 (8%) | 532.6 (21%) |
| 100.00% | **179.2 (9%)** | 620.3 (8%) | 904.3 (3%) | 914.5 (9%) | 1,087.1 (3%) | 186.6 (4%) | 1,599.9 (2%) | 538.3 (11%) |

Heavy redraw:

| changed | c-poll | c-rowstamp | c-blockstamp | c-collector | c-bevy | c-unity | compiled | compiled-scan |
|---|---|---|---|---|---|---|---|---|
| 0.01% | 920.6 (1%) | 2,043.3 (4%) | 271.9 (1%) | **253.0 (1%)** | 2,143.2 (1%) | 924.9 (1%) | 277.3 (1%) | 1,979.3 (3%) |
| 0.10% | 921.0 (1%) | 2,038.8 (1%) | 614.6 (2%) | **274.8 (0%)** | 2,141.0 (0%) | 924.1 (1%) | 331.4 (2%) | 1,977.5 (1%) |
| 1.00% | 920.6 (0%) | 2,046.1 (1%) | 2,006.3 (2%) | **432.5 (10%)** | 2,147.4 (1%) | 923.1 (0%) | 592.4 (4%) | 1,969.1 (3%) |
| 10.00% | **920.1 (1%)** | 2,040.9 (1%) | 2,090.8 (1%) | 1,574.1 (1%) | 2,142.4 (2%) | 925.5 (0%) | 1,726.4 (1%) | 1,975.3 (4%) |
| 100.00% | **917.7 (1%)** | 2,043.3 (0%) | 2,192.6 (3%) | 13,226.1 (0%) | 2,150.0 (1%) | 924.3 (1%) | 2,995.6 (0%) | 1,977.8 (1%) |

Parallel variants, light redraw:

| changed | compiled-par, baseline | compiled-par-walk1, baseline | compiled-par-scan, baseline | compiled-par, native | compiled-par-walk1, native | compiled-par-scan, native |
|---|---|---|---|---|---|---|
| 0.01% | **59.7 (1%)** | 60.3 (1%) | 119.5 (2%) | **46.2 (3%)** | 46.7 (2%) | 63.7 (3%) |
| 0.10% | 73.0 (1%) | **66.3 (1%)** | 122.2 (3%) | 64.3 (3%) | **56.0 (1%)** | 64.5 (5%) |
| 1.00% | 165.4 (4%) | **97.3 (0%)** | 127.6 (8%) | 171.0 (5%) | 94.9 (10%) | **64.3 (4%)** |
| 10.00% | 260.6 (4%) | 258.3 (3%) | **122.1 (4%)** | 234.6 (6%) | 234.5 (9%) | **64.4 (2%)** |
| 100.00% | 305.4 (3%) | 305.1 (2%) | **122.6 (3%)** | 272.9 (2%) | 271.8 (1%) | **64.7 (5%)** |

Heavy redraw:

| changed | compiled-par, baseline | compiled-par-walk1, baseline | compiled-par-scan, baseline | compiled-par, native | compiled-par-walk1, native | compiled-par-scan, native |
|---|---|---|---|---|---|---|
| 0.01% | 62.0 (2%) | **60.9 (1%)** | 84.1 (1%) | 48.1 (1%) | **46.8 (1%)** | 255.3 (4%) |
| 0.10% | 85.7 (1%) | **66.7 (1%)** | 92.4 (2%) | 79.2 (4%) | **57.6 (2%)** | 256.5 (3%) |
| 1.00% | 276.4 (3%) | **105.4 (1%)** | 133.9 (4%) | 289.2 (4%) | **101.8 (8%)** | 256.6 (4%) |
| 10.00% | 370.0 (4%) | 367.1 (4%) | **221.9 (2%)** | 335.0 (6%) | 340.0 (3%) | **254.3 (1%)** |
| 100.00% | 1,861.9 (0%) | 1,861.2 (0%) | **1,692.7 (0%)** | 449.1 (2%) | 450.7 (1%) | **255.5 (3%)** |

### What holds

- The instruction set is worth 2.5x in the caches and nothing out of
  them: the fused frame takes 119 against 299 ns at 1e3, 1.76x less at
  1e4, 1.12x at 1e5, 1.07x at 1e6 and the same at 1e7. Native, it takes
  0.37-0.39x the Mac's time at 1e3-1e4 and still 1.9-2.7x at 1e6-1e7.
- The generated frame taking 15% longer than C at 1e3 was baseline SSE2:
  pinned it is 17% (299 against 256 ns), native it is gone, and generated
  fused code matches C within 2% at every size.
- SSE2 was why the reactive scan lost to C. Native, `compiled-scan` with
  light work takes 0.84-0.87x the time of `c-rowstamp` at every rate
  (526-540 against 620-635 us; the Mac: 6-19% faster), where baseline it
  took 1.56x.
- SSE2 was not why churn's compiled form lost its lead: the lead follows
  the caches, not the instruction set. In the
  caches (1e5, no churn) it leads the hand-written select in both builds:
  9% per system and 17% fused baseline (30.7 and 28.0 against 33.8 us),
  2% and 14% native (17.5 and 15.3 against 17.8 us). At 1e6, where the
  21 MB world exceeds the 16 MB L3, it does not: baseline 2-3% slower per
  system and 4% faster fused, native 4% and 8% slower (386 and 402 against
  370 us), the fused form slower than the unfused one.
- With masked stores the `if` is no longer the slow form. Native,
  `wide-branch` takes 17.1-17.4 us at 1e5 at every density, 3-4% less than
  `wide-select`, where baseline it took up to 7.3x (248 against 34 us at
  50% density); at 1e6 it takes 1.23x. "A C programmer's natural `if` is
  the slow one" holds for NEON and SSE2, not for AVX-512.
- The same turns the reactive results around. Native, clang vectorises
  the check-first loops by doing the work for every unit: `c-rowstamp`
  with heavy work takes 2.04 ms at every rate, 6.4x less than baseline
  with all changed (13.2 ms) and 3.3x more with 0.01% changed (619 us);
  `compiled-scan` likewise (1.98 ms at every rate). That is the masked
  heavy form the 2026-09-30 entry listed as not measured, chosen by LLVM
  rather than by the lowering, and it costs the scan its advantage at low
  rates.
- So polling wins far more often: native `c-poll` takes 178 us light at
  every rate, less than every sequential tracking scheme (251 us at best),
  and 0.92 ms heavy, less than every scheme from 10% changed on. The
  collector, whose loop cannot vectorise, wins heavy work up to 1% (253-433
  us) and stays at 13.2 ms with all changed.
- The compiled event log beating the collector with few changes was SSE2
  too: native the collector leads by 11-24% up to 1% light (251 against
  279 us at 0.01%), as on the Mac.
- In parallel, native, scanning beats the log from 1% changed with light
  work (64 us at every rate against 171-273) and from 10% with heavy work
  (255 against 335-449 us); the log wins below (46-48 us at 0.01%).
  Walking the log in parallel however few entries are pending
  (`compiled-par-walk1`) is within 1.5% of the default threshold or
  faster at every rate in both builds (97 against 165 us at 1% light, baseline):
  on this machine, where forks are cheap, `parallel-min-events` should be
  far lower than 16,384.
- Binding 12 threads to 12 cores changes nothing for the frame out of the
  caches: fused parallel takes 359 us at 1e6 and 10.4 ms at 1e7 (unbound:
  393 us with 16% spread, 10.6 ms), 2.6x and 1.15x less than sequential.
  Native does not help there either (377 us, 10.5 ms).
- Bound, the reactive parallel variants spread 0-8% (unbound: up to 66%)
  and take half the time with few changes (60 against 127 us at 0.01%
  light). The unbound run used the runtime's default thread count and
  started at load average 16.5, so this does not separate binding from
  thread count from load. With all changed and heavy work the bound scan
  is slower (1.69 against 1.36 ms).

### Measured, not explained

- Why the compiled churn form trails the hand-written select once the
  world leaves the L3, and why fusing then costs (402 against 386 us
  native at 1e6) where it gains 9-13% in the caches.
- `c-bevy` with light work is 1.7x slower native at 0.01% changed (1,073
  against 617 us) and about as slow at every rate, and `compiled-scan` with light work spreads 11-21%
  native against 1-4% baseline.
- Archetype moves still win without churn at 1e6 at every density in both
  builds; at 1e5, native, the compiled fused form wins at 90% (15.3
  against 16.5 us) and loses at 50% (15.2 against 13.9).
- The buffered apply costing twice `c-index`, and parallel push losing to
  sequential push, were not rerun.

## 2026-10-06: trees, a river network

`examples/river.ent`: nodes in a tree (`relation Flows ... tree`), rain on
each, and every node's flow the rain on everything upstream of it: a `for`
that cascades `leaves first` and adds its flow into its parent's
(`down.flow += n.flow`, `down: mut Node up Flows`). `bench/river/run.py`
runs it against hand-written C. Ryzen AI 9 HX 370, Linux, the pixi
toolchain (LLVM 22.1.8), `taskset -c 2` (a Zen 5 core), `-march=native`
(from this entry on what the scripts build with unless `BENCH_CFLAGS` says
otherwise), 200 steps after 50, medians of 7 processes.
Checksums (every flow's bits) agree in every configuration. Load average
1.3-1.5 at the start.

Shapes: `bushy`, node i flows into a node picked from all before it;
`deep`, into one of the 8 before it; `shuffled`, bushy with the nodes in a
random order. C variants: `c-order` walks a list of the nodes with a
parent, parents before children, from its end, and finds each node's
parent by the node's number; `c-pairs` has the parent next to the node in
the list, which is what the generated code does; `c-sorted` stores the
nodes themselves in that order.

us per step (spread), best in bold; `ent-sorted` is the same program with
its tree `sorted` (see the next entry):

| nodes | shape | depth | c-order | c-pairs | c-sorted | ent | ent-sorted |
|---|---|---|---|---|---|---|---|
| 1e4 | bushy | 21 | 6.1 (3%) | 4.3 (3%) | **3.5 (3%)** | 5.4 (2%) | 3.6 (5%) |
| 1e4 | deep | 2,192 | 9.0 (2%) | 8.4 (2%) | 8.2 (1%) | **8.1 (2%)** | 8.5 (2%) |
| 1e4 | shuffled | 21 | 6.0 (1%) | 4.2 (2%) | **3.5 (2%)** | 5.4 (1%) | 3.6 (2%) |
| 1e5 | bushy | 25 | 76.2 (11%) | 48.0 (2%) | **36.1 (2%)** | 56.3 (3%) | 36.4 (3%) |
| 1e5 | deep | 21,920 | 108.6 (4%) | 81.2 (1%) | 82.6 (3%) | **81.1 (1%)** | 85.6 (2%) |
| 1e5 | shuffled | 25 | 74.5 (10%) | 48.5 (2%) | **35.8 (2%)** | 56.0 (4%) | 36.4 (4%) |
| 1e6 | bushy | 31 | 1,365.3 (14%) | 827.1 (19%) | **378.6 (1%)** | 842.3 (14%) | 387.7 (1%) |
| 1e6 | deep | 219,241 | 1,124.2 (1%) | 841.2 (1%) | **839.2 (1%)** | 853.4 (10%) | 872.3 (2%) |
| 1e6 | shuffled | 31 | 1,306.6 (9%) | 958.6 (23%) | **379.5 (0%)** | 980.3 (15%) | 389.2 (2%) |

### How it got here

us per step at 1e6 bushy, 1e5 bushy and 1e4 bushy, a run of 7 rounds
each:

| generated code | 1e6 | 1e5 | 1e4 |
|---|---|---|---|
| each node's parent found through its edge (two offsets, then the target) | not measured native | | |
| the parent next to the node in the list | 1,093 | 71.5 | 6.8 |
| ... and the node found without checking its id | 842 | 56.3 | 5.4 |

The first form was only measured in a baseline (SSE2) build, 5 rounds of
100 steps after 10: 3,292, 155.5 and 11.0, where the second took 1,023,
69.3 and 6.6.

The first form made three scattered loads per node before it reached a
flow; the list is read in order. The id needs no check where the
relation's sources cannot die and can live in one archetype only.

### What holds

- Against the same storage and the same lists (`c-pairs`), the generated
  code takes 0.96-1.02x the time at 1e6 and in deep trees at every size,
  and 1.15-1.26x in bushy trees that fit the caches (1e4, 1e5).
- A deep tree is a chain of dependent adds: every form but `c-order`
  takes 0.81-0.87 ns per node at 1e5 and 1e6, whatever its storage.
- Where the nodes are stored matters more than anything the generated
  code does: `c-sorted` takes 0.40-0.46x the time of `c-pairs` at 1e6
  bushy and shuffled, and 0.74-0.81x at 1e4 and 1e5. ent-lang stores
  entities in the order they were spawned.
- The order the nodes were spawned in makes no difference to the forms
  that walk a list (bushy against shuffled): the list is in neither.

### Measured, not explained

- The 1.15-1.26x over `c-pairs` in the caches. The generated loop computes
  each list position from the end (`count - 1 - i`) and masks the row out
  of each id, where the C counts down and uses the numbers as they are;
  whether that is the difference was not checked.
- The spreads of 9-23% at 1e6 bushy and shuffled in every form that does
  not store the nodes in order.

### Not measured

- Trees whose nodes are in several archetypes, or whose sources can die
  (ids checked, the parent searched for where the relation does not say
  what its targets have).
- Parents first (`cascade` without `leaves first`), and anything in
  parallel: a cascading `for` runs on one core.

## 2026-10-06: a sorted tree

`relation Flows from Node to Node tree sorted capacity N` (measured while
it was still written on the archetype, `sorted by Flows`; the generated
code is the same) keeps the rows of the archetype holding the tree's
entities in the tree's order: the entities without a parent, then the others
breadth first, each with its parent's row in a column. A query cascading
along the tree is then a loop over rows, and reads its parent at that row.
The rows are put in order by the relation's sort, which now also runs when
the archetype gains or loses an entity. Same machine, pinning and flags as
the entry above, whose table has the steady state (`ent-sorted`, the same
run as the other columns).

### What holds

- The generated code takes 1.01-1.04x the time of `c-sorted`, the same
  storage by hand, in every cell.
- Against the unsorted program: 0.40-0.46x at 1e6 bushy and shuffled,
  0.65-0.67x at 1e4 and 1e5, and 1.02-1.06x (slower) in deep trees, where
  the sum is one chain of dependent adds and the order of rows gains
  nothing.

### What a change to the tree costs

`--resort`: one node is connected to the node it already flows into
before every step, so every step sorts the edges, and a sorted archetype
its rows, first. ms per step (spread), 5 rounds of 50 steps after 10, and
in brackets what one sort costs: the step less the step without it.

| nodes | shape | ent | ent-sorted |
|---|---|---|---|
| 1e4 | bushy | 0.053 (3%) [0.048] | 0.063 (1%) [0.059] |
| 1e5 | bushy | 1.05 (2%) [0.99] | 1.22 (1%) [1.18] |
| 1e5 | deep | 0.55 (1%) [0.47] | 0.71 (1%) [0.62] |
| 1e6 | bushy | 18.8 (3%) [17.9] | 24.2 (3%) [23.8] |
| 1e6 | deep | 6.3 (1%) [5.4] | 8.9 (2%) [8.1] |
| 1e6 | shuffled | 17.2 (5%) [16.2] | 21.9 (2%) [21.5] |

- Sorting the edges is most of it, sorted archetype or not: 16-18 ms at
  1e6 bushy and shuffled, 17-21 steps' worth for the unsorted program.
- Putting the rows in order adds 2.7-5.9 ms at 1e6 (two fields and the
  ids), 19-50% on top of the edges' sort at every size.
- At 1e6 bushy a step is 0.45 ms shorter sorted and a change 5.9 ms
  dearer: sorted storage pays where the tree changes less often than every
  13 steps or so. In deep trees it never pays.

### Not measured

- Spawning or destroying nodes, which sorts a sorted archetype the same
  way (and does not sort the edges of an unsorted program at all).
- Archetypes with more or wider columns, which all move.
- What ids that are slots rather than rows cost the rest of a program:
  every lookup then goes through the entity table. The river looks nothing
  up.
- Why the sorted form is 2-6% slower in deep trees.

## 2026-10-06: a sorted tree across archetypes

A sorted tree may now have its entities in several archetypes. Each keeps
its rows by depth in the tree and notes where each depth starts; a row has
its parent's location, archetype and row. A cascading query goes depth by
depth and, in each, through the rows every archetype has of it; a tree in
one archetype keeps its one loop over rows. `ent-two` and `ent-two-sorted`
are the river with half its nodes (picked by a hash of their number) in a
second archetype with one more component. Same machine, pinning and flags;
200 steps after 50, medians of 7 processes, one run for all columns.
`ent-two` agrees with the C on the checksum; `ent-two-sorted` adds a
node's inflows in another order (by archetype, then row) and agrees on the
sum of all flows to 1e-4.

us per step (spread):

| nodes | shape | depth | c-sorted | ent | ent-sorted | ent-two | ent-two-sorted |
|---|---|---|---|---|---|---|---|
| 1e4 | bushy | 21 | 3.5 (5%) | 5.4 (2%) | 3.6 (2%) | 8.0 (4%) | 4.4 (4%) |
| 1e4 | deep | 2,192 | 8.2 (2%) | 8.1 (2%) | 8.6 (2%) | 8.7 (8%) | 9.7 (5%) |
| 1e4 | shuffled | 21 | 3.5 (4%) | 5.5 (1%) | 3.6 (3%) | 8.0 (4%) | 4.4 (2%) |
| 1e5 | bushy | 25 | 36.4 (2%) | 56.5 (2%) | 36.8 (2%) | 438.5 (4%) | 44.9 (4%) |
| 1e5 | deep | 21,920 | 82.6 (2%) | 81.6 (2%) | 85.3 (2%) | 374.7 (4%) | 185.3 (3%) |
| 1e5 | shuffled | 25 | 36.0 (2%) | 56.5 (5%) | 36.8 (4%) | 437.0 (3%) | 44.9 (3%) |
| 1e6 | bushy | 31 | 378.1 (2%) | 931.2 (15%) | 389.5 (2%) | 4,841.1 (3%) | 474.6 (2%) |
| 1e6 | deep | 219,241 | 841.6 (2%) | 855.2 (6%) | 874.0 (2%) | 3,683.0 (5%) | 1,635.3 (3%) |
| 1e6 | shuffled | 31 | 378.4 (3%) | 968.1 (30%) | 390.9 (3%) | 4,871.8 (3%) | 477.0 (4%) |

With `--resort` (5 rounds of 50 steps after 10), ms per step at 1e6 and in
brackets what one sort costs:

| shape | ent-two | ent-two-sorted |
|---|---|---|
| bushy | 24.0 (3%) [19.2] | 37.4 (3%) [36.9] |
| deep | 13.5 (1%) [9.8] | 19.0 (1%) [17.4] |
| shuffled | 23.4 (5%) [18.6] | 34.4 (1%) [33.9] |

### What holds

- Sorted across two archetypes, a bushy tree takes 1.22x the time of the
  same tree sorted in one, at every size: the branch on the parent's
  archetype, and two loops a depth.
- A deep tree takes 1.9-2.2x at 1e5 and 1e6: with 219,241 depths for 1e6
  nodes, most loops run for two or three rows.
- Unsorted, the tree across two archetypes is the slow one: 4.3-7.8x the
  time of the unsorted tree in one archetype at 1e5 and 1e6 (1.1-1.5x at
  1e4). So sorting gains more here than in one archetype: 0.10x the time
  in a bushy tree at 1e5 and 1e6, 0.44-0.49x in a deep one.
- A sort costs 34-37 ms at 1e6 bushy and shuffled, against 19 unsorted:
  both archetypes' rows move, 15-18 ms on top of the edges. With 4.4 ms
  saved a step, the order pays where the tree changes less often than
  every 4 steps.

### Measured, not explained

- Why the unsorted tree in two archetypes costs 4-5 ns a node at 1e5 and
  1e6 and under 1 ns at 1e4. Each node's archetype and its parent's are
  told by branches there, which follow no pattern across the list; a
  predictor that learns a sequence of 1e4 and not one of 1e5 would fit.
  Not checked.

### Not measured

- More than two archetypes, and archetypes of very different sizes.
- A component of the ancestor that not every archetype has (searched for
  from the entity, as unsorted).

## 2026-10-06: the unsorted tree across archetypes, without its checks

The entry above left the unsorted river in two archetypes at 4.3-7.8x the
time of the one in one archetype. Its loop told each node's archetype, and
its parent's, by a branch, and then checked the row against the
archetype's count, which it loaded: the shortcut for an entity known to be
alive and to have the component (a trusted end of an edge, an id from the
tree's list) was only taken where it could live in one archetype. Now
such an entity is found in any number of archetypes by a branch on its
archetype alone, the last without a test. Same machine, pinning and flags;
200 steps after 50, medians of 7 processes, one run. Checksums agree with
the C.

us per step (spread); `ent-two` before is the entry above:

| nodes | shape | c-pairs | ent | ent-two before | ent-two | ent-two-sorted |
|---|---|---|---|---|---|---|
| 1e4 | bushy | 4.3 (6%) | 5.4 (6%) | 8.0 | 6.6 (1%) | 4.4 (3%) |
| 1e4 | deep | 8.5 (3%) | 8.2 (1%) | 8.7 | 8.2 (2%) | 9.7 (5%) |
| 1e4 | shuffled | 4.3 (2%) | 5.4 (2%) | 8.0 | 6.7 (5%) | 4.4 (2%) |
| 1e5 | bushy | 48.7 (2%) | 56.3 (3%) | 438.5 | 67.8 (3%) | 44.9 (3%) |
| 1e5 | deep | 82.4 (1%) | 81.6 (2%) | 374.7 | 82.0 (2%) | 185.2 (2%) |
| 1e5 | shuffled | 48.5 (2%) | 56.4 (2%) | 437.0 | 67.7 (2%) | 45.2 (5%) |
| 1e6 | bushy | 941.0 (20%) | 934.1 (26%) | 4,841.1 | 1,094.4 (13%) | 473.7 (2%) |
| 1e6 | deep | 844.8 (2%) | 862.0 (4%) | 3,683.0 | 877.8 (3%) | 1,631.5 (2%) |
| 1e6 | shuffled | 951.5 (20%) | 973.7 (16%) | 4,871.8 | 1,128.2 (13%) | 476.5 (1%) |

### What holds

- The unsorted tree in two archetypes takes 1.16-1.23x the time of the one
  in one archetype when bushy and 1.00-1.02x when deep: 0.15-0.23x of what
  it took at 1e5 and 1e6.
- That the branches and checks were the cost is what taking them out
  shows; what they cost each was not taken apart. The generated loop now
  has no load but the ids, the parent's and the flows.
- Sorting across archetypes is left with less to gain: 0.42-0.43x the time
  at 1e6 bushy and shuffled, 0.66x at 1e4 and 1e5, and in a deep tree it
  loses, 1.2x at 1e4 and 1.9-2.3x at 1e5 and 1e6. At 1e6 bushy it saves
  0.62 ms a step and a sort costs 17.7 ms more (the entry above), so it
  pays where the tree changes less often than every 28 steps or so.

### Not measured

- A tree whose entities can die, or whose relation does not name what its
  ends have: there the checks stay, and with them, presumably, the cost.

## 2026-10-06: what a sort costs, and when it runs

Two changes to the relations' sort, and one that was tried and left out.
Same machine, pinning and flags.

**A system's connects are sorted once.** A connect outside a query sorted
its relation at once, each of them a pass over every key and edge: a
system that builds a tree of n entities paid n sorts. Now the relation is
sorted before the system's next query and when the system ends. A chain
of nodes spawned and connected in a `world` block, with capacities of
32,768, the whole program's time:

| nodes | before | after |
|---|---|---|
| 4,000 | 0.22 s | 0.001 s |
| 8,000 | 0.54 s | |
| 16,000 | 0.89 s | 0.002 s |

**A tree's edges are put in order in one pass.** It sorted them by source
with a counting sort, dropped all but the last edge of each source, and
sorted again for the offsets. Now it notes each source's last edge and
goes through the keys in order. With `--resort` at 1e6 nodes, 7 rounds of
50 steps after 10, ms a sort costs (the step with it less the step
without; before is the two entries above):

| shape | ent before | ent | sorted before | sorted | two before | two | two sorted before | two sorted |
|---|---|---|---|---|---|---|---|---|
| bushy | 17.9 | 17.4 | 23.8 | 23.1 | 19.2 | 18.3 | 36.9 | 34.8 |
| deep | 5.4 | 4.0 | 8.1 | 6.8 | 9.8 | 6.0 | 17.4 | 15.4 |
| shuffled | 16.2 | 15.2 | 21.5 | 20.8 | 18.6 | 16.9 | 33.9 | 32.0 |

### What holds

- When a sort runs mattered far more than what it costs: building a tree
  in ent-lang took time by the product of its size and the capacities.
- The one pass saves 1.4-3.8 ms in a deep tree (16-39%) and 0.5-2.1 ms
  in a bushy one (3-6%), which is within the spreads there (3-5%).
- In a bushy tree the edges' order was never the cost. Sampling the
  program counter of the generated sort (a timer in the process; no
  `perf` here) puts about a third of it in the loop that lists the tree
  breadth first, at the loads of each listed entity's range of children
  and of the children, which are wherever the parent's key puts them.

### Tried, and left out

- Copying four children whatever their number, where an entity has at
  most four, instead of a loop over them that is mispredicted for most
  entities, with the children's ids kept next to each other: by hand it
  halved the listing (15.6 to 7.0 ms of a sort written out in C). In the
  generated sort it gave 17.0 against 18.2 ms in a bushy tree and 5.8
  against 4.8 in a deep one. Not worth its code.

### Not done

- A sort that does only what a change needs. Every connect, and every
  spawn or destroy in a sorted archetype, still sorts everything: 4-35 ms
  at 1e6 nodes.

## 2026-10-06: a tree that takes a change in where it happens

A tree that is not `sorted` no longer keeps a table of edges that is
sorted for every change. Every entity key has a slot for its entity's one
edge, the edges to an entity are a list through their sources' slots, and
the list a cascading query walks is kept as the program connects: a new
leaf goes to its end, a node put under one that is before it (or under one
without a parent) has its parent changed in place. What that cannot do (a
node put under one that comes after it, a node with children given a
parent, a disconnect, edges a host connected) has the tree built again
from its slots, breadth first. So the order a tree is visited in is the
order it was connected in, where it was the tree's shape alone. Same
machine, pinning and flags; 1e6 nodes, 5 rounds.

us per step (spread), 200 steps after 50; checksums agree with the C:

| shape | c-pairs | ent | ent-sorted | ent-two | ent-two-sorted |
|---|---|---|---|---|---|
| bushy | 835.6 (10%) | 991.5 (22%) | 406.5 (6%) | 1,127.1 (14%) | 499.2 (14%) |
| deep | 847.1 (11%) | 873.0 (4%) | 873.9 (2%) | 879.8 (2%) | 1,692.0 (6%) |
| shuffled | 992.2 (22%) | 1,006.1 (13%) | 410.1 (6%) | 1,143.1 (14%) | 501.4 (6%) |

ms per step with one node connected before every step to the node it
already flows into, 50 steps after 10: by the program (`--divert`, the
river's schedule `divert`), and by the host (`--resort`):

| shape | ent, program | ent, host | ent-sorted, program | ent-sorted, host | ent-two, program | ent-two, host |
|---|---|---|---|---|---|---|
| bushy | 0.98 (92%) | 27.0 (10%) | 23.2 (16%) | 23.6 (1%) | 1.05 (27%) | 28.8 (1%) |
| deep | 0.88 (3%) | 4.1 (7%) | 7.7 (4%) | 7.9 (10%) | 0.88 (4%) | 8.2 (6%) |
| shuffled | 0.94 (24%) | 27.4 (14%) | 21.0 (4%) | 21.6 (3%) | 1.13 (20%) | 29.8 (14%) |

### What holds

- A change the program makes to an unsorted tree that its list can take
  in costs nothing that shows in a step of 1e6 nodes: 0.88-1.13 ms with
  it, the step's own time, where the entry above has 4.9-18.3.
- Steps without a change take what they took.
- A sorted tree is built again for every change, as before: 21-23 ms in a
  bushy tree.
- Building an unsorted tree again costs more than sorting its edges did
  when the tree is bushy: 26 ms against 17.4 (a host's connect, less the
  step), and less when it is deep, 3.3 against 4.0. Each child is now
  found through its sibling's slot, one load after another.

### Not measured

- A change that has the tree built again from the program (a node put
  under a later one): it should cost what the host's connect does.
- Edge loops over a tree, which now go from child to child through the
  slots, against the ranges of a table.
- What the slots and links cost in memory: 32 bytes an entity key and
  the fields, whatever the tree's size.

## 2026-10-06: building an unsorted tree again

The entry above left building an unsorted bushy tree again at 26 ms for
1e6 nodes, against 17.4 for the sort it replaced. Sampling the program
counter put nearly half of it in the loop that listed the tree breadth
first: for every listed entity a branch on whether it has children, which
is not to be foreseen, taken only once the entity's links have come from
memory. An unsorted tree's list need not be breadth first any more, only
parents before children. Now the entities are listed by key, each after
those of its ancestors that are not in yet (kept, while they wait, at the
end of the list's own array), and a child joins its parent's children
without a branch on whether it is the first. Same machine, pinning and
flags; 1e6 nodes, a host's connect before every step, 5 rounds of 50
steps after 10; ms a build costs (the step with it less the step
without):

| shape | the sort (two entries above) | breadth first (the entry above) | by key |
|---|---|---|---|
| bushy | 17.4 | 26.1 | 11.4 |
| deep | 4.0 | 3.3 | 3.5 |
| shuffled | 15.2 | 26.4 | 19.7 |
| bushy, two archetypes | 18.3 | 27.7 | 18.6 |
| deep, two archetypes | 6.0 | 7.3 | 7.8 |

### What holds

- A bushy tree whose nodes were made parents first is built again in 0.66x
  the time the sort took, and 0.44x of breadth first.
- Where the nodes' keys say nothing about the tree (shuffled), most
  parents are not in yet when their child's turn comes, and the branch
  that tells is as hard to foresee as the one that went: 1.3x the sort.
- In two archetypes there are twice the keys to go through, and it takes
  what the sort took (bushy) or 1.3x (deep).

### Tried, and left out

- Every entity's children put next to each other by counting, and the
  links made from that: 28.9 ms in a bushy tree, no better than chasing
  the siblings' links, since the listing stalled on the same branch.

### Not measured

- A tree connected leaves first, where every entity waits for all its
  ancestors.

## 2026-10-06: a node put under a later one

An unsorted tree was built again when a node was put under one that comes
after it in the tree's order, or when a node that has children got a
parent. Now such a node goes to the end of the order with everything below
it (itself, then the children of each node moved, so the order's end is
the queue), and its old entries stay behind as gaps a walk skips. The
order has room for twice the relation's edges, and is made again when
that runs out. Same machine, pinning and flags; 1e6 nodes, 5 rounds.

us per step, 200 steps after 50, checksums agreeing with the C; since the
entry above, a tree a host has built is listed by key:

| shape | c-pairs | ent | ent-two |
|---|---|---|---|
| bushy | 906.4 (19%) | 675.8 (2%) | 863.3 (2%) |
| deep | 845.0 (2%) | 845.3 (2%) | 988.6 (3%) |
| shuffled | 906.3 (18%) | 818.7 (9%) | 984.6 (12%) |

us per step with `--move`, 50 steps after 10: before every step the
program puts node 1000 + s under node 999 + s, which the step before put
at the order's end, so a node with about 1,000 below it moves each step:

| shape | ent | ent-sorted | ent-two |
|---|---|---|---|
| bushy | 914.4 (3%) | 23,294.9 (1%) | 1,105.1 (3%) |
| shuffled | 827.4 (10%) | 20,934.2 (2%) | 981.8 (16%) |

### What holds

- A step that moves a node with about 1,000 below it takes 0.24 ms longer
  than a step alone in the bushy tree (914 against 676) and no longer in
  the shuffled one, where building the tree again cost 11.4 and 19.7 ms.
  How much of the 0.24 ms is the move and how much the walk over an order
  that is less in order was not taken apart.
- The list by key is also the better one to walk: the unsorted bushy tree
  takes 0.75x the time of `c-pairs`, which walks the nodes breadth first,
  where it took as long before (902 to 991 against 836 to 951).

### Not measured

- Moves in a deep tree: what is below a node there is most of the tree,
  and the scheme above ties it into a cycle.
- A move of so much of a tree that building it again would be cheaper.
  Going by the 0.24 ms, that is somewhere near a twentieth of a bushy
  tree's nodes; nothing looks at the size before moving.

## 2026-10-06: edge loops over a tree

A tree that is not `sorted` keeps a slot per entity and links from child
to child, where other relations have a table of edges. `bench/river/
edges.py` runs edge loops over the river's tree both ways: `edges.ent` as
it is (`ent-tree`), and with `tree` taken out (`ent-table`), against C by
hand. `pull`: every node sums the rain on the nodes that flow straight
into it, along the edges in. `push`: every node sends its rain to the node
it flows into, along its edge out. Same machine, pinning and flags; 200
steps after 50, medians of 7 processes. Checksums agree in every
configuration.

us per step (spread), best in bold:

| nodes | shape | way | c | ent-tree | ent-table |
|---|---|---|---|---|---|
| 1e4 | bushy | pull | **5.8 (25%)** | 9.2 (2%) | 7.1 (4%) |
| 1e4 | bushy | push | **2.8 (4%)** | 3.5 (3%) | 6.8 (12%) |
| 1e4 | deep | pull | **5.9 (11%)** | 6.4 (10%) | 6.7 (3%) |
| 1e4 | deep | push | **3.4 (1%)** | 4.0 (2%) | 7.0 (9%) |
| 1e4 | shuffled | pull | **5.6 (15%)** | 10.3 (1%) | 7.5 (4%) |
| 1e4 | shuffled | push | **2.9 (2%)** | 3.6 (4%) | 6.9 (3%) |
| 1e5 | bushy | pull | **374.9 (1%)** | 383.1 (2%) | 386.3 (1%) |
| 1e5 | bushy | push | **33.7 (1%)** | 41.8 (2%) | 73.8 (2%) |
| 1e5 | deep | pull | 70.1 (9%) | **65.9 (9%)** | 67.7 (2%) |
| 1e5 | deep | push | **33.9 (1%)** | 40.9 (2%) | 69.5 (19%) |
| 1e5 | shuffled | pull | **513.2 (1%)** | 532.3 (4%) | 541.4 (3%) |
| 1e5 | shuffled | push | **35.2 (2%)** | 43.5 (1%) | 75.3 (1%) |
| 1e6 | bushy | pull | **4,013.7 (3%)** | 4,478.8 (9%) | 4,344.0 (3%) |
| 1e6 | bushy | push | **509.1 (2%)** | 645.4 (4%) | 1,001.5 (2%) |
| 1e6 | deep | pull | **634.3 (10%)** | 666.5 (7%) | 750.5 (5%) |
| 1e6 | deep | push | **366.1 (15%)** | 434.0 (11%) | 744.2 (11%) |
| 1e6 | shuffled | pull | **5,618.8 (1%)** | 6,541.3 (1%) | 6,168.8 (4%) |
| 1e6 | shuffled | push | **568.9 (20%)** | 708.4 (8%) | 1,071.6 (3%) |

### What holds

- Along the edge out, the tree's slot takes 0.51-0.66x the time of the
  table (one load of the slot's owner and one of the target, where the
  table has two offsets and then the target), and 1.18-1.27x of the C,
  whose loop the generated one is but for the owner it compares.
- Along the edges in, from child to child, the tree takes 0.89-1.06x the
  time of the table at 1e5 and 1e6, and 1.3-1.4x in a bushy or shuffled
  tree of 1e4, where everything is in the caches and the next child is
  one more load to wait for; in a deep tree it is 0.96x there.
- Pull costs 4-6 ns a node in a bushy tree in every form, the C too: how
  many nodes flow into one is not to be foreseen, and the rain is
  wherever the node above is.

### Tried, and left out

- Carrying the visited entity's own field through the walk from child to
  child as a value, as a table's loop does: 5,035 against 4,455 us at 1e6
  bushy, 392 against 382 at 1e5. No gain.

### Not measured

- Edge loops that write the edges' fields, or disconnect.
- Entities that can die, where every edge in is checked for its target.

## 2026-10-06: a tree whose nodes can be destroyed

A program that destroys entities has ids with generations, found through
the entity table, and nothing about an edge's ends was taken on trust: a
cascading query checked the generation of every listed entity and of its
parent, and looked the parent up twice, once to find the ancestor and once
to combine into it. `ent-mortal` is the river with a system that destroys
nodes (which the host never runs), `ent-two-mortal` the same in two
archetypes. Now a despawned entity's edge and the edges to it are taken
out of an unsorted tree with the entity, so its edges' ends are always
alive, and trusted as those of a tree whose nodes cannot go are. Same
machine, pinning and flags; 200 steps after 50, medians of 5 processes;
checksums agree.

us per step (spread):

| nodes | shape | ent | ent-mortal before | ent-mortal | ent-mortal-sorted | ent-two | ent-two-mortal before | ent-two-mortal | ent-two-mortal-sorted |
|---|---|---|---|---|---|---|---|---|---|
| 1e5 | bushy | 46.7 (1%) | 111.2 | 84.1 (11%) | 37.1 (2%) | 62.9 (2%) | 468.2 | 85.8 (8%) | 45.4 (1%) |
| 1e5 | deep | 81.0 (1%) | 100.4 | 90.4 (2%) | 86.0 (1%) | 93.2 (2%) | 342.9 | 93.9 (0%) | 185.1 (1%) |
| 1e5 | shuffled | 55.1 (2%) | 144.6 | 92.6 (15%) | 37.4 (4%) | 66.2 (5%) | 632.2 | 108.2 (15%) | 45.6 (1%) |
| 1e6 | bushy | 687.6 (4%) | 3,878.4 | 1,991.2 (7%) | 391.7 (1%) | 870.2 (3%) | 7,765.4 | 2,156.5 (9%) | 475.4 (0%) |
| 1e6 | deep | 845.0 (6%) | 1,067.3 | 967.1 (4%) | 877.1 (0%) | 990.8 (3%) | 3,689.8 | 1,004.2 (2%) | 1,630.2 (3%) |
| 1e6 | shuffled | 823.9 (4%) | 3,946.8 | 2,129.0 (10%) | 390.6 (1%) | 996.7 (13%) | 9,258.4 | 2,335.4 (8%) | 476.5 (1%) |

### What holds

- In one archetype the tree whose nodes can go takes 0.51-0.54x the time
  it took at 1e6 bushy and shuffled, and 0.64-0.76x at 1e5; in two
  archetypes 0.25-0.28x at 1e6 and 0.17-0.18x at 1e5.
- It still takes 2.3-2.9x the time of the tree whose nodes cannot go at
  1e6 bushy and shuffled (1.4-1.8x at 1e5, 1.0-1.1x deep): its ids are
  slots, and the row of every node and of its parent comes from the
  entity table.
- Sorted, it takes what the sorted tree of nodes that cannot go takes:
  rows, and each parent's row, with no id on the way. For a tree whose
  nodes can go, that is 0.20x (one archetype) and 0.22x (two) of the
  unsorted at 1e6 bushy.

### Not measured

- Destroying nodes: each takes its own edge and its children's out of the
  tree, in time by the number of its children.
- A tree whose relation does not name what its ends have, or whose
  program removes that component: there the checks stay.

## 2026-10-07: rows without the entity table, for a tree whose nodes can go

In a program that destroys entities an id is a slot, and a cascading
query over an unsorted tree read the entity table twice a node: for the
node's row and for its parent's. The tree's list now has, next to each
entity's id and its parent's, the packed location of both, so a walk goes
to the rows from the list. The locations are noted when an entry is made.
When rows of an archetype that can hold the tree's entities move (a
despawn's swap, a move to another archetype, a sorted archetype put in
order) the tree is marked stale, and where its relation is next looked
over the locations are read from the entity table again, once. Same
machine, pinning and flags; 200 steps after 50, medians of 5 processes;
checksums agree.

us per step (spread); before is the entry above:

| nodes | shape | ent | ent-mortal before | ent-mortal | ent-mortal-sorted | ent-two | ent-two-mortal before | ent-two-mortal |
|---|---|---|---|---|---|---|---|---|
| 1e5 | bushy | 46.7 (3%) | 84.1 | 52.0 (2%) | 37.0 (3%) | 62.3 (4%) | 85.8 | 59.7 (2%) |
| 1e5 | deep | 80.9 (2%) | 90.4 | 81.6 (3%) | 86.0 (1%) | 93.0 (1%) | 93.9 | 86.6 (2%) |
| 1e5 | shuffled | 55.5 (2%) | 92.6 | 61.9 (3%) | 37.5 (4%) | 66.0 (1%) | 108.2 | 70.8 (2%) |
| 1e6 | bushy | 685.5 (8%) | 1,991.2 | 907.2 (9%) | 391.4 (2%) | 863.8 (2%) | 2,156.5 | 1,087.8 (6%) |
| 1e6 | deep | 855.4 (4%) | 967.1 | 876.8 (3%) | 873.6 (1%) | 982.5 (9%) | 1,004.2 | 909.1 (9%) |
| 1e6 | shuffled | 823.1 (24%) | 2,129.0 | 1,011.8 (20%) | 392.2 (3%) | 952.0 (12%) | 2,335.4 | 1,197.6 (17%) |

### What holds

- The tree whose nodes can go takes 0.46-0.51x the time it took at 1e6
  bushy and shuffled, and 0.62-0.70x at 1e5.
- It is left at 1.2-1.3x of the tree whose nodes cannot go at 1e6 bushy
  and shuffled, 1.0-1.1x at 1e5, and what that one takes when deep: a
  location to take apart per row, and two more columns of the list to
  read.
- Since the first entry on such trees it is 0.23-0.26x in one archetype
  and 0.13-0.14x in two (3,878 and 7,765 us at 1e6 bushy).

### Not measured

- A step in which rows moved: the locations of the whole list are then
  read from the entity table once, which should cost about what a step
  cost before this entry less what it costs now, 1 ms at 1e6.

## 2026-10-07: a sorted tree's depths in parallel

The entities of one depth of a tree do not depend on each other. A sorted
tree in one archetype has its rows by depth; its sort now also notes where
each depth starts and, for every row, the rows of its children, which are
next to each other. With `parallel-entities=1` a cascading query whose
body is local to a depth goes depth by depth where a depth holds 256
entities on average, and runs a depth of at least `parallel-min-level`
entities (32,768) in parallel. The river combines into the parent, so a
depth's rows are cut into 64 pieces at rows where a parent's children
begin, and the pieces run in parallel, each in order. `ent-sorted-par` is
that (with `parallel-min-entities=1`, which also makes the loop that
sets every node's flow parallel). Same machine and flags; 200 steps after
50, medians of 7 processes, `OMP_NUM_THREADS=12 OMP_PLACES=cores
OMP_PROC_BIND=close`, nothing pinned to one core. Checksums agree: the
sums are the same to the bit.

us per step (spread):

| nodes | shape | depth | c-sorted | ent-sorted | ent-sorted-par |
|---|---|---|---|---|---|
| 1e5 | bushy | 25 | 39.2 (67%) | 38.5 (147%) | 66.0 (21%) |
| 1e5 | deep | 21,920 | 85.7 (29%) | 85.2 (171%) | 120.2 (3%) |
| 1e5 | shuffled | 25 | 56.5 (62%) | 44.0 (147%) | 72.6 (24%) |
| 1e6 | bushy | 31 | 376.0 (9%) | 388.6 (3%) | 182.2 (15%) |
| 1e6 | deep | 219,241 | 840.2 (2%) | 867.8 (19%) | 937.5 (1%) |
| 1e6 | shuffled | 31 | 376.1 (3%) | 388.3 (9%) | 180.1 (14%) |

With `OMP_NUM_THREADS=4` (the Zen 5 cores, by `OMP_PLACES=cores`), 5
rounds, 1e6 bushy: 144.8 (3%) against 396.0 (29%).

### What holds

- At 1e6 nodes in a bushy tree the parallel form takes 0.47x the time of
  the one loop with 12 threads, and 0.37x with 4: the four fast cores do
  better alone than with the eight slower ones.
- A deep tree is not gone through by depth (four or five nodes a depth)
  and takes 1.08x: the price of the other loop of the step being
  parallel, here forced on.
- At 1e5 it loses, 1.4-1.7x: a step of 40-85 us is less than forking
  costs. With the default `parallel-min-entities` of 1e6 a world of that
  size has no parallel loop.

### How it got here

- First, the parents of a depth in parallel, each with a loop over its
  children: 446 us at 1e6 bushy, no better than the one loop's 392. That
  measurement was of nothing, though: the test for a wide tree asked for
  32,768 nodes a depth on average, which a tree of 1e6 nodes and 31
  depths just misses, so the one loop ran, and the 446 were the other
  loop's forks.
- The loop per parent was replaced, unmeasured, by pieces of rows cut at
  parents' boundaries, which read the rows as the one loop does.

### Not measured

- A tree across archetypes, which stays on one core.
- A body with more work per node than an add, which is where more cores
  have more to gain: the river's step is two streams through memory.
- A query that only reads its parent (parents first, nothing combined),
  whose depths run over their rows directly.

## 2026-10-07: a reactive `for` that cascades (scene graph)

`bench/scene/run.py`: every node's place is its parent's plus its own
offset (`w.x = above.x + l.x`, i32). `full` places every node every step
with a cascading `for`; `reactive` is the same `for` with `on changed
Local, changed World up Under`, which runs its body for the nodes the
host moved in the step and for what is below them. `-sorted`: the tree
`sorted`. Native build, one core (`taskset -c 2`), 5 processes of 100
steps each; `reached` is the nodes below the moved ones, themselves
included, summed over the moves of a step (a node below two of them
counts twice). All variants agree on a checksum of every node's place.

Over the unsorted tree the reactive `for` goes from the events down the
children's links (a bitmap over the tree's list, marked from the event
logs and by each node the body changes, and gone through in the list's
order). Over the sorted one it goes through every row and runs its body
where a trigger fired, which is what it did over both when first built.

us per step, median (spread):

| nodes | shape | moves | reached | full | reactive | full-sorted | reactive-sorted |
|---|---|---|---|---|---|---|---|
| 1e5 | bushy | 0 | 0 | 37.4 (2%) | **0.5 (1%)** | 22.9 (1%) | 30.9 (5%) |
| 1e5 | bushy | 1 | 6 | 38.4 (4%) | **0.9 (5%)** | 22.9 (3%) | 31.6 (2%) |
| 1e5 | bushy | 10 | 63 | 38.4 (3%) | **3.7 (12%)** | 23.1 (6%) | 31.9 (5%) |
| 1e5 | bushy | 100 | 1,096 | 37.6 (4%) | 37.0 (4%) | **23.0 (1%)** | 37.5 (7%) |
| 1e5 | bushy | 1000 | 10,965 | 38.6 (4%) | 148.3 (1%) | **24.2 (3%)** | 76.6 (2%) |
| 1e5 | deep | 0 | 0 | 43.5 (3%) | **0.5 (10%)** | 37.4 (1%) | 30.9 (2%) |
| 1e5 | deep | 1 | 11,146 | 44.1 (2%) | 69.9 (2%) | **37.4 (1%)** | 47.5 (10%) |
| 1e5 | deep | 10 | 110,699 | 43.7 (2%) | 145.6 (2%) | **37.6 (1%)** | 121.8 (0%) |
| 1e5 | deep | 100 | 1,089,217 | 43.8 (2%) | 199.2 (2%) | **37.6 (1%)** | 168.4 (1%) |
| 1e5 | deep | 1000 | 11,016,950 | 45.0 (7%) | 212.1 (2%) | **38.5 (1%)** | 181.1 (1%) |
| 1e6 | bushy | 0 | 0 | 645.0 (4%) | **3.3 (0%)** | 241.8 (5%) | 384.3 (3%) |
| 1e6 | bushy | 1 | 5 | 626.4 (14%) | **5.0 (1%)** | 243.8 (12%) | 389.4 (2%) |
| 1e6 | bushy | 10 | 57 | 667.2 (10%) | **18.5 (10%)** | 233.8 (5%) | 390.3 (3%) |
| 1e6 | bushy | 100 | 894 | 653.4 (10%) | **140.1 (3%)** | 238.1 (3%) | 427.4 (1%) |
| 1e6 | bushy | 1000 | 20,431 | 656.3 (4%) | 1,652.5 (4%) | **240.3 (1%)** | 693.9 (4%) |
| 1e6 | deep | 0 | 0 | 459.3 (15%) | **3.3 (1%)** | 376.8 (1%) | 362.5 (5%) |
| 1e6 | deep | 1 | 96,024 | 463.3 (15%) | 599.8 (5%) | **377.2 (6%)** | 513.5 (3%) |
| 1e6 | deep | 10 | 1,221,576 | 455.4 (15%) | 1,615.9 (1%) | **379.2 (3%)** | 1,285.4 (1%) |
| 1e6 | deep | 100 | 10,575,314 | 451.6 (12%) | 2,006.6 (1%) | **379.9 (3%)** | 1,692.6 (1%) |
| 1e6 | deep | 1000 | 109,895,143 | 453.3 (6%) | 2,066.4 (2%) | **383.1 (1%)** | 1,784.7 (1%) |

### What holds

- Following the events, the time is the moves': at 1e6 nodes in a bushy
  tree 3.3 us with nothing moved, 5.0 with
  one node, 140 with a hundred, against 630-670 for placing everything:
  0.005x, 0.008x, 0.2x.
- A moved node costs some 1.4 us at 1e6 (0.35 at 1e5), with the nine
  nodes or so it reaches: every one of them is somewhere else in every
  column, and the pass over all of them reads on at 0.6 ns a node. So
  the reactive `for` wins up to about 400 moves a step at 1e6 bushy (a
  node in a hundred placed again) and loses beyond: 2.5x at 1000 moves.
- In the deep tree one move reaches a tenth of the nodes, and the
  reactive `for` takes 1.3x the full one; with most of the tree placed
  again, 3.5-4.5x. There the events are more than their logs hold and
  it goes through the whole tree, paying for the ticks and for the
  events it writes on top of the body.
- Over the sorted tree, which it goes through row by row, it takes
  1.6x the full `for` with nothing moved (0.96x in the deep tree) and
  never less than about that.

### How it got here

- First built going through the whole list everywhere: 1,337-1,475 us
  at 1e6 bushy for 0-100 moves, twice the full `for`.
- Following then showed 2,720 us in the deep tree with 100 moves where
  going through the list had taken 1,290, and looking for why found
  that an event log could lose events unnoticed: an event that found
  its log full was told only to the reader that had read least of it.
  Fixed (`test/Integration/reactive_lost.test`; any reactive `for`
  sharing a log with another could miss events after an overflow).
  With that, the deep tree goes through the list when its events
  overflow, at 2,007 us: the 700 more than before are the events of
  `World`, which nothing read then and which are written now.
- A rule that goes through the list when more than a sixteenth of it
  has events waiting is in, and decides nothing in these runs: the logs
  (an eighth of the nodes) overflow before it.

### Not measured

- A body with real work, where the reactive form wins further up.
- The sorted tree following events (its rows' child ranges would
  serve): not built.
- A second level over the bitmap, which would take the 3.3 us of an
  idle step at 1e6 to nothing: not built.
- Larger event logs (`log N`) for the deep tree.

## 2026-10-07: a sorted tree follows events too

The same benchmark, after a reactive `for` over a tree sorted in one
archetype was made to go where the events lead as well: the marks are a
bit per row, and a row's children are the rows from its child range's
begin to its end. Native, one core (`taskset -c 2`), 3 processes of 100
steps each, 1e6 nodes; all variants agree on the checksum (also at 1e4
and 1e5).

us per step, median (spread):

| shape | moves | reached | full | reactive | full-sorted | reactive-sorted |
|---|---|---|---|---|---|---|
| bushy | 0 | 0 | 622.3 (3%) | 3.3 (1%) | 233.6 (2%) | 3.3 (1%) |
| bushy | 1 | 5 | 635.2 (6%) | 5.1 (2%) | 234.3 (8%) | **4.3 (2%)** |
| bushy | 10 | 57 | 650.4 (7%) | 18.4 (1%) | 242.5 (0%) | **9.7 (0%)** |
| bushy | 100 | 894 | 632.9 (6%) | 140.1 (2%) | 235.3 (3%) | **63.2 (0%)** |
| bushy | 1000 | 20,431 | 653.8 (5%) | 1,653.9 (4%) | **239.3 (0%)** | 638.8 (3%) |
| deep | 0 | 0 | 461.2 (10%) | 3.3 (0%) | 379.8 (2%) | 3.3 (1%) |
| deep | 1 | 96,024 | 450.3 (3%) | 588.6 (0%) | **376.5 (2%)** | 453.2 (1%) |
| deep | 10 | 1,221,576 | 447.9 (2%) | 1,606.2 (0%) | **378.1 (1%)** | 1,350.7 (0%) |
| deep | 100 | 10,575,314 | 450.7 (2%) | 2,018.3 (0%) | **381.4 (1%)** | 1,746.3 (1%) |
| deep | 1000 | 109,895,143 | 451.2 (3%) | 2,082.6 (1%) | **385.9 (2%)** | 1,835.2 (1%) |

### What holds

- The sorted tree's reactive `for` was 384-427 us for 0-100 moves in the
  bushy tree and is 3.3-63: with one node moved, 0.018x of placing every
  row, and with a hundred 0.27x.
- A moved node costs about 0.6 us, under half the unsorted tree's 1.4:
  its children are rows next to each other, not links to follow.
- It wins up to about 350 moves a step (the full `for` over a sorted
  tree is the fastest thing there is to beat), and loses beyond: 2.7x at
  1000 moves, 1.2x-4.8x in the deep tree, as before.
- An idle step takes 3.3 us over either tree, with a bitmap of half the
  words for the sorted one: so that is not the bitmap. Not looked into.

### Not measured

- A sorted tree across several archetypes, which still goes through
  every row.

## 2026-10-07: what reacting to a new parent costs

A trigger up a tree now also fires for an entity that was connected to
another parent, for which the body's test reads one more tick per
entity, kept by the relation per entity key. The scene graph again
(`bench/scene/run.py`, native, `taskset -c 2`, 3 processes of 100 steps,
1e6 nodes; nothing is connected in it, so this is the price alone).
Reactive variants, us per step, before -> after:

| shape | moves | reactive | reactive-sorted |
|---|---|---|---|
| bushy | 0 | 3.3 -> 3.3 | 3.3 -> 3.3 |
| bushy | 1 | 5.1 -> 5.3 | 4.3 -> 4.4 |
| bushy | 100 | 140.1 -> 151.5 | 63.2 -> 71.6 |
| bushy | 1000 | 1,653.9 -> 1,776.7 | 638.8 -> 1,091.9 |
| deep | 1 | 588.6 -> 634.0 | 453.2 -> 550.2 |
| deep | 100 | 2,018.3 -> 2,212.5 | 1,746.3 -> 2,000.0 |

- 4-13% where the `for` follows events, since an entity it comes to is
  a jump anyway.
- More where it goes through every row of a sorted tree (1000 moves in
  the bushy tree, 1.7x; the deep tree, 1.15-1.2x): the tick is kept by
  entity key, and a sorted tree's rows are in another order than the
  keys, so that one read jumps where the others read on. A tick per row
  would not; not built.
- Not measured: a step that does connect nodes.

## 2026-10-07: three follow-ups on reactive and sorted trees

`bench/scene/run.py` at 1e6 nodes, native; one core (`taskset -c 2`)
unless said, 3 processes of 100 steps (5 for the parallel runs). us per
step, medians.

### The tick of a connect per row

A sorted archetype keeps a copy per row of when each entity was last
connected (the relation has it per entity key), made where the rows are
put in order. `reactive-sorted`, before connects were events -> with the
tick by key -> with the copy:

| shape | moves | us |
|---|---|---|
| bushy | 100 | 63.2 -> 71.6 -> 65.9 |
| bushy | 1000 | 638.8 -> 1,091.9 -> 685.8 |
| deep | 1 | 453.2 -> 550.2 -> 514.2 |
| deep | 100 | 1,746.3 -> 2,000.0 -> 1,895.9 |

Most of what reacting to a new parent cost a sorted tree is back; the
rest is one more column read per row.

### A query that changes more than its logs hold

Two changes to event logs. A lost event (the log is full for a reader)
moved the segment's count and noted it, each time; now once, until a
reader has finished with the log. And an event's segment was its row's
share of the rows, a division per event; one loop takes the row's low
bits (a parallel one keeps the shares, which keep its threads apart).

| shape | moves | reactive | reactive-sorted | full | full-sorted |
|---|---|---|---|---|---|
| deep | 100 | 2,212.5 -> 1,793.2 | 1,895.9 -> 1,610.1 | 454.8 | 380.6 |
| deep | 1000 | 2,283.2 -> 1,843.4 | 1,970.2 -> 1,657.6 | 452.3 | 386.0 |
| bushy | 1000 | 1,776.7 -> 1,729.4 | 685.8 -> 633.0 | 666.0 | 241.3 |

- Telling of lost events once took 2,283 to 1,808; the segment without
  a division made no difference that shows (1,843, within the spread),
  and is kept for what it does to a run of rows that change together,
  which now fills the whole log before it overflows and not a 64th.
- It is still 4x the query without `on` where most of a deep tree is
  placed again. What is left is the reactive query's own work per
  entity (three ticks read, one written, an event logged) on top of a
  body of one add; where the 1,300 us go among those was not measured.
  No rule that switches to the full walk helps here: the full walk of a
  reactive query is this.

### A sorted tree across archetypes

Half the nodes in a second archetype (`-two-`). Parallel: 12 threads,
`OMP_PLACES=cores OMP_PROC_BIND=close`, not pinned, where the one-core
numbers of the same run are in brackets (they are noisier unpinned).

| shape | full-sorted | full-sorted-par | full-two-sorted | full-two-sorted-par |
|---|---|---|---|---|
| bushy | 242.8 [452.5] | 121.3 | 287.9 [532.9] | 226.3 |
| deep | 375.3 [395.7] | 403.8 | 1,440.9 [1,462.2] | 1,419.5 |

`reactive-two-sorted`, going through every depth's rows -> following
events (a mark per row of each archetype, each row with its children's
rows in each, gone through depth by depth):

| shape | moves | us | full-two-sorted |
|---|---|---|---|
| bushy | 0 | 611.0 -> 12.0 | 290.9 |
| bushy | 1 | 606.6 -> 13.1 | 289.2 |
| bushy | 100 | 641.3 -> 97.9 | 290.2 |
| bushy | 1000 | 970.7 -> 888.7 | 298.1 |
| deep | 0 | 1,435.8 | 1,439.9 |
| deep | 1 | 1,823.8 | 1,439.9 |
| deep | 100 | 2,912.7 | 1,434.8 |

- A depth in parallel across two archetypes: 0.79x of one pinned core in
  the bushy tree, where one archetype gets 0.50x. Each archetype has half
  of a depth, so fewer depths reach the 32,768 rows that are worth a
  fork. Nothing in the deep tree, as before.
- Following events: 13 us for a moved node where the full query takes
  289; it wins up to about 300 moves a step.
- An idle step is 12 us, not the one archetype's 3.3: 31 depths times two
  archetypes of ranges to look at.
- The deep tree (219,241 depths) is not followed: fewer than 64 nodes a
  depth, and going through the depths costs more than the rows. It goes
  through every row, as it did, at the full query's time or more.
- Not measured: the parallel form with a body of more than an add; a
  tree in more than two archetypes.

## 2026-10-07: three more on reactive and sorted trees

1e6 nodes, native; one core (`taskset -c 2`) unless said; us per step,
medians of 3 processes (5 for the river).

### Where a reactive query's time goes when most of a tree changes

`bench/scene/run.py`, deep tree, 1000 moves a step (every pass goes
through the whole list and places most nodes), with parts of the
lowering switched off one at a time (the results are then wrong; only
the times count):

| | reactive | reactive-sorted |
|---|---|---|
| as it was | 1,800 | 1,596 |
| without appending to event logs | 1,155 | 986 |
| and without the tick of each entity's last connect | 957 | 762 |
| and without writing ticks at all | 626 | 515 |
| the query without `on` | 457 | 389 |

So: 645 us events that find their log full, 200 the connect ticks, 330
writing ticks, 170 reading them and the masked body. Two changes from
that:

- An event looks first whether the log's readers have already been told
  of a lost one (and none has finished with the log since): then it is
  lost too, and nothing else of the log is read.
- The relation keeps the tick of its latest connect of any entity; a
  query in whose time nothing was connected looks at no entity's.

| shape | moves | reactive | reactive-sorted |
|---|---|---|---|
| deep | 100 | 1,793.2 -> 1,442.5 | 1,610.1 -> 1,303.7 |
| deep | 1000 | 1,843.4 -> 1,478.0 | 1,657.6 -> 1,339.7 |
| deep | 1 | 596.0 -> 545.9 | 529.7 -> 439.4 |
| bushy | 1000 | 1,729.4 -> 1,606.2 | 633.0 -> 579.0 |
| bushy | 100 | 146.1 -> 139.3 | 64.1 -> 61.2 |

4.1x the query without `on` has become 3.3x. Less than the parts
promised (957): the first eighth of the events of a pass is still
appended, and every later one still looks. What is left is mostly the
ticks, written and read, which a reactive query cannot do without.

### A deep tree sorted across archetypes, by its list

Going depth by depth through 219,241 depths, each with a start to look
up in every archetype, cost more than the rows. A tree with fewer than
64 nodes a depth is now gone through by the tree's list, with each
entity's row and its parent's next to it.

| | one archetype | two, by depth | two, by list |
|---|---|---|---|
| scene, `full-*-sorted`, deep | 375.3 | 1,440.9 | 651.5 |
| river, `ent-*-sorted`, deep | 882.1 | (about 2x, 2026-10-06) | 897.8 |

- The scene graph (parents first, reading the parent) is at 1.7x of one
  archetype, the river (children first, adding into the parent) at 1.02x.
- A reactive query went the other way, 1,436 us by depth and 2,944 by
  the list with nothing moved, and keeps to the depths. Not understood.

### A depth in parallel across archetypes, adding into ancestors

`bench/river/run.py`, bushy, 12 threads (`OMP_PLACES=cores
OMP_PROC_BIND=close`, not pinned), 5 processes:

| ent-sorted | ent-sorted-par | ent-two-sorted | ent-two-sorted-par |
|---|---|---|---|
| 399.7 (394.7 pinned) | 185.8 | 477.6 (481.7 pinned) | 308.4 |

- One archetype's rows of a depth are cut into 64 pieces at rows where a
  parent's children begin, and the archetypes take their turns as when
  one core does it: checksum and total are the same to the bit as the
  one-core run's (bushy and deep).
- 0.64x of one pinned core with two archetypes, 0.47x with one: each
  archetype has half of a depth, and fewer depths are worth a fork.
- The deep tree goes by its list on one core (972 against 887: the
  other loop of the step being parallel).

## 2026-10-07: an order asked for exactly

`top down bfs R` and `top down dfs R` (and `bottom up`) give a `for` an
exact order along a tree. A sorted or ordered tree is stored breadth
first; any other exact order is worked out from the tree's list every
time the `for` runs (how many are below each node, then a place for
each), and the nodes are then found by their ids. The scene graph,
placing every node (`bench/scene/run.py`, bushy, native, `taskset -c 2`,
3 processes of 100 steps); all agree on the checksum.

us per step, median (spread):

| nodes | full | full-bfs | full-dfs | full-sorted | full-sorted-dfs |
|---|---|---|---|---|---|
| 1e4 | 3.3 (2%) | 41.3 (1%) | 21.4 (1%) | 2.3 (1%) | 24.0 (1%) |
| 1e6 | 634.4 (5%) | 17,201.9 (4%) | 10,294.2 (4%) | 239.5 (2%) | 16,352.1 (2%) |

- Depth first costs 6x the any-order walk at ten thousand nodes and 16x
  at a million; breadth first over a tree that is not stored that way
  12x and 27x (it is made from the depth-first order). Over a sorted
  tree depth first is 68x at a million: its rows are by depth, and the
  order jumps among them.
- Most of it is working the order out, which is the same every step
  here: the tree does not change. Keeping the order until it does is
  not built.

## 2026-10-07: an exact order, kept until the tree changes

The order worked out for `bfs` or `dfs` is kept with the tree and worked
out again only after a connect, a destroyed node or a rebuilt order.
The same runs as above (the tree does not change in them), us per step,
before -> after:

| nodes | full | full-bfs | full-dfs | full-sorted | full-sorted-dfs |
|---|---|---|---|---|---|
| 1e4 | 3.3 | 41.3 -> 4.6 | 21.4 -> 7.1 | 2.3 | 24.0 -> 6.8 |
| 1e6 | 653.7 | 17,201.9 -> 1,266.4 | 10,294.2 -> 1,236.5 | 239.4 | 16,352.1 -> 2,834.5 |

- With the order at hand, an exact order takes 1.9x the any-order walk
  at a million nodes (1.4-2.2x at ten thousand): the entities are found
  by their ids, one lookup each.
- Depth first over a sorted tree is 12x the sorted walk: the rows are
  by depth, and depth first jumps among them.
- A step that changes the tree pays what the first table says, once.
- One order is kept per tree. Not measured: two queries asking the same
  tree for different orders, which each work theirs out every step.

## 2026-10-07: a layout that follows its events

`bench/boxes/boxes.ent`: 90,301 boxes laid out by `examples/layout.ent`
(300 columns of 300 in a row, each column as high as its boxes), one box
in the middle of a column another height every frame. The column and the
row are fitted again and the 150 boxes below are placed again; nothing
else has anything to do. The Linux machine, one Zen 5 core,
`ENT_CFLAGS=-march=native tools/ent run -I examples
bench/boxes/boxes.ent`: us a frame over 200 frames, three runs each.

| | us a frame |
|---|---|
| every pass goes through the tree, running its body where a trigger fired | 1,664 - 1,678 |
| the passes follow their events | 196 - 197 |
| and only new boxes are asked whether they have what `layout` keeps | 17.8 - 18.2 |

- The passes react to a child's size (from the leaves), to the box
  before and to the box they are in. None of them was followed before:
  a trigger on a child or a sibling was not, nor one on a parent's
  component that not every parent has (`Stack`, `Content`), which now
  is where the arrow is to the parent itself.
- What was left then was the library's first `for`, which gives a box
  what `layout` keeps if it has not got it: over every box, every
  frame. With `on added Box` it is over the new ones.
- 83 times less than going through everything, for a change that
  reaches some 450 of 90,000 boxes. What 18 us are made of is not
  measured: eight passes each look through a mark per box (1,400 words).
- The first line was measured with following switched off in the
  compiler for the run, which is not kept. A frame in which a box comes
  or goes is such a frame: that children or siblings changed is in no
  log, and all are asked.
- Later the same day: a frame in which a box comes or goes is followed
  too (the tree keeps a ring of the last 256 entities that got other
  children or siblings), and placing and floating run until a round
  places nothing instead of three rounds: 17.6 - 17.8 us a frame here.

## 2026-10-08: how long the layout demo takes to build

`tools/ent run -I examples examples/layout_demo.ent` took 21 s before
its window showed, 16 of them in `clang -O2` on one function: `layout`,
430,000 lines of LLVM IR. A pass that follows its events had its body
where it goes through everything, where it follows and where it visits
the boxes without a parent, each time once for each of nine archetypes;
and every field read through an arrow looked its entity up again.

| | LLVM IR, lines | to LLVM IR | clang -O2 |
|---|---|---|---|
| before | 457,830 | 5.0 s | 16.0 s |
| a body is one function for its places | 322,447 | 2.6 s | 6.5 s |
| the entity an arrow leads to is found once | 263,863 | 2.5 s | 5.7 s |
| and one function for all archetypes, given their columns | 113,132 | 1.2 s | 5.8 s |
| which are given as places in the world's memory, not as views | 61,045 | 0.8 s | 1.0 s |

- 1.8 s in all now. The parts are found after lowering: what is emitted
  in several places is named where it is emitted, and the parts of a
  name that are alike but for the values they take from outside become
  one function with those as parameters.
- A column given as a view is five values to LLVM, and a body takes a
  hundred of them: giving the offsets instead was most of the last step.
- At run time the same: `bench/boxes` 18.0 - 18.9 us a frame (17.2 -
  17.4 before), the scene graph's `reactive` 1.2 us for one move and 157
  for a thousand (1.1 and 174).
- Later: the spawns of a function that are alike are one function too
  (`world_setup` 19,036 lines to 14,354; the build 1.6 s). Tried and
  not kept: the same for the append to an event log, which every write
  of a watched field has. 8,700 lines fewer, no faster to build, and
  `bench/boxes` went from 18 to 22 - 23 us a frame.
- A program that was built is kept (`tools/ent`, see its --help): the
  demo starts in 0.4 s the second time.

## 2026-10-08: the layout library with Clay's sizing

`examples/layout.ent` sizes boxes as Clay does now (percent, limits,
growing the smallest first and shrinking the largest first, padding
side by side, alignment both ways, ratios, a box's own content): five
passes where there were three, and every box has six components of the
library's own.

- `bench/boxes`: 24.0 - 25.5 us a frame (18 before).
- The demo's first build: 5.0 s (1.6 before; started again it takes 0.4
  from the cache). It was 10 s with the new library as first written:
  a body that reads fields of the boxes inside a box in eight loops
  looked each box up, by a branch for each of fifteen archetypes, for
  every field. Now an entity is found once for the fields read of it,
  and by a table: where it is says, by a chain of selects, where each
  column starts, and one load reads it (469,178 lines of LLVM IR to
  197,361). Bodies of plain reactive `for`s and of those that go in an
  order worked out are one function for all archetypes too (165,639).
- What is left is mostly the calls of those functions, each given some
  fifty places in the world's memory for one archetype.
