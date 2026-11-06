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
| 10⁴ | 10,527 | 0.164 | 64,045 | 0.196 | 53,776 |
| 10⁵ | 105,486 | 4.482 | 23,536 | 2.511 | 42,006 |
| 10⁶ | 1,054,530 | 107.052 | 9,851 | 79.108 | 13,330 |

Where the time goes at 10⁵ (`--trace`, one core): the hash index `ByUrl`
is made anew 246 times (1.4 s) and `Kept` 123 times (0.7 s), since every
frame spawns asks and a hash index is made again when rows move or come.
Mending it in place, as relation indexes are, is the next thing to try
(spec §10.4). Before the duplicates went through `get` on those indexes
(a `for` inside a `for` uses none), 10⁵ pages took 138 s.

At 10⁴ all cores are slower than one: the frames are many and small.
Pages a second fall with size (64k, 24k, 10k on one core) because the
indexes are made anew each frame over all asks so far: the work grows
with pages times frames, not pages.
