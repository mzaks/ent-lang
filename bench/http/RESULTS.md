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
| 10⁴ | 10,527 | 0.123 | 85,369 | 0.194 | 54,349 |
| 10⁵ | 105,486 | 2.007 | 52,559 | 1.997 | 52,829 |
| 10⁶ | 1,054,530 | 25.547 | 41,278 | 24.643 | 42,792 |

Before, in seconds (one core / all cores):

| pages | C1 as committed (e828767) | hash indexes mended (1b94e5a) |
|---|---|---|
| 10⁴ | 0.164 / 0.196 | 0.128 / 0.187 |
| 10⁵ | 4.482 / 2.511 | 2.086 / 2.147 |
| 10⁶ | 107.052 / 79.108 | 62.047 / 72.668 |

At 10⁵ (`--trace`, one core) the two hash indexes `ByUrl` and `Kept`
took 2.5 s of 4.5 made anew each frame, 0.11 s mended. Before the
duplicates went through `get` on those indexes (a `for` inside a `for`
uses none), 10⁵ pages took 138 s.

Then at 10⁶ it was the relation `Links`: 30.1 s of 52.3 (one core) in
its sort, of which the sorts anew were little (20 of them, two a
level, where a level's links are more than a sixteenth of those so
far). The rest was the mending before them (devices/edges.c): where a
run of edges is full it looks for room in the runs next to it, and that
walk was held to a budget of edges moved but not of keys passed; most
of the 2M entity keys have no links, so each edge of a level walked far
over empty runs before the mending gave up. Every key passed now counts
as an edge moved (the relation's churn bench, bench/graph, is as it
was). What is left at 10⁶ is mostly texts: making and giving back the
slices of the link scan (`ent_text_make`, `ent_text_release`, 37 % of
the time).

At 10⁴ all cores are slower than one: the frames are many and small.
Pages a second fall with size because the sorts anew of the links (two
a level) grow with the pages so far.
