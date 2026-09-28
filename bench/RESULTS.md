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
