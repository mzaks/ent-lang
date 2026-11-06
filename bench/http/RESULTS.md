# HTTP crawler benchmarks

`examples/http`, spec §9. Native builds (`-march=native`), on mains
power, Ryzen AI 9 HX 370 (12 cores, 24 threads), the CPU clamp checked
first. Each report is checked against the site's expected one before it
is timed; the median of three runs.

## 9.1 Mock throughput (C1)

`python3 bench/http/mock.py`: `crawl_mock.ent` on a generated site
(`site.py --depth 1000`, every page reachable fetched), on the mock
device: no I/O, the clock jumps to the next thing due. The time is the
whole run: reading the site's manifest, the crawl, the report. One core
is CPU 2 (`taskset`); all cores is a `--parallel` build.

| pages | fetched | one core s | pages/s | all cores s | pages/s |
|---|---|---|---|---|---|
| 10⁴ | 10,527 | 0.128 | 82,250 | 0.187 | 56,303 |
| 10⁵ | 105,486 | 2.086 | 50,580 | 2.147 | 49,140 |
| 10⁶ | 1,054,530 | 62.047 | 16,996 | 72.668 | 14,512 |

Before hash indexes were mended where entities come, go or move (C1 as
committed, e828767):

| pages | one core s | all cores s |
|---|---|---|
| 10⁴ | 0.164 | 0.196 |
| 10⁵ | 4.482 | 2.511 |
| 10⁶ | 107.052 | 79.108 |

At 10⁵ (`--trace`, one core) the two hash indexes `ByUrl` and `Kept`
took 2.5 s of 4.5 made anew each frame, 0.11 s mended. Before the
duplicates went through `get` on those indexes (a `for` inside a `for`
uses none), 10⁵ pages took 138 s.

At 10⁶ what is left is the relation `Links`: sorted anew 362 times,
30.1 s of 52.3 (one core), though relations are mended in place where
edges come (docs/indexes.md, step 6): to be looked into. Then the
link scan (`extract_links`, 6.1 s) and `settle` (1.9 s).

At 10⁴ all cores are slower than one: the frames are many and small.
Pages a second fall with size because what is sorted each frame (the
links) grows with the pages so far.
