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

The mock bench's sites have a robots.txt ask since C3 (a 404): one page
more fetched than in the tables above (10⁴ checked: 0.129 s one core).

## 9.1 Live against C (C3)

`python3 bench/http/live.py`: `crawl_live.ent` (examples/http/live.ent on
devices/http, libcurl) against `bench/http/crawl.c` (the same crawler in
C on curl multi: the same rules, URL and robots.txt fns one for one, the
same report), each over the network to `serve.c` on 127.0.0.1 (four
threads, CPUs 4 to 11), through curl's proxy. The site is site.py's
(`--depth 1000`), its delays a tenth as long (a plain page 0 to 1 ms, a
slow one 150 ms, the slowest 500 ms) but for slow and timed out (1 s,
2 s), so no page is near either and the report does not hang on how long
a frame takes. `per_host` and the connections (to the host and in all)
are the column's. ent is a one-core build on CPU 2, ent-par a
`--parallel` build on CPUs 0 to 3, C on CPU 2. Every report is checked
first; the median of three runs (its CPU time, user and system, and peak
memory with it).

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.017 | 0.123 | 23 | 10,352 |
| 10⁴ | 64 | ent | 1.142 | 0.378 | 68 | 9,218 |
| 10⁴ | 64 | ent-par | 1.161 | 3.638 | 68 | 9,066 |
| 10⁴ | 256 | C | 1.053 | 0.146 | 23 | 9,995 |
| 10⁴ | 256 | ent | 1.136 | 0.300 | 68 | 9,271 |
| 10⁴ | 256 | ent-par | 1.130 | 3.342 | 70 | 9,317 |
| 10⁴ | 1024 | C | 1.068 | 0.209 | 23 | 9,860 |
| 10⁴ | 1024 | ent | 1.168 | 0.350 | 69 | 9,016 |
| 10⁴ | 1024 | ent-par | 1.157 | 3.058 | 71 | 9,102 |
| 10⁵ | 64 | C | 2.258 | 1.158 | 71 | 46,725 |
| 10⁵ | 64 | ent | 22.719 | 21.929 | 502 | 4,643 |
| 10⁵ | 64 | ent-par | 21.081 | 71.487 | 506 | 5,004 |
| 10⁵ | 256 | C | 2.056 | 1.337 | 75 | 51,317 |
| 10⁵ | 256 | ent | 8.540 | 8.053 | 465 | 12,351 |
| 10⁵ | 256 | ent-par | 8.167 | 27.183 | 461 | 12,916 |
| 10⁵ | 1024 | C | 3.122 | 2.415 | 76 | 33,787 |
| 10⁵ | 1024 | ent | 5.501 | 4.945 | 460 | 19,175 |
| 10⁵ | 1024 | ent-par | 5.960 | 13.889 | 461 | 17,698 |

At 10⁴ every crawler waits on the network and the levels (about 1 s
whatever it does), ent with three times C's CPU. At 10⁵ C waits too and
ent does not: it works the whole time, and the fewer the connections,
the longer, since a frame takes the answers of at most so many and
costs the more the more asks there are. At 64 connections the crawl is
4,223 frames (the mock's is 123), and what each costs grows with the
asks so far, not with what came in it (`--trace`, one core, 21.4 s):

- keeping `Links` in order, 6.6 s: a frame's edges are more than a
  sixteenth of those so far, or come from pages with no room, often
  enough that it is sorted anew;
- `settle`, 3.0 s: a page that waits for its level (answered, links not
  yet to be taken) is looked at again every frame, `is_html` lowering
  its headers each time;
- `receive`, 2.2 s (polling and waiting, not the crawler's work);
- `be_polite` 1.6 s and `pace` 0.9 s: every ask not sent, sorted by
  serial, every frame;
- the hash indexes 1.8 s, `extract_links` 1.0 s.

So the shape of the pipeline is not what costs (C does the same steps)
but systems that look at every waiting ask each frame, which the mock,
jumping from one thing due to the next, hid. The cures: a tag for what a
system has to look at (a page waiting for its level once, not every
frame), asks waiting by host in a queue rather than all sorted each
frame, `Links` mended where new pages' runs have no room. ent-par uses
three to four times the CPU for the same wall time: its threads wait on
the main thread's procs. C's peak memory is a seventh of ent's (the
capacities for 10⁵ asks, links and history are made at the start).

### Live, after the first per-frame fixes

The same bench after these changes. Each was traced at 10⁵ pages and 64
connections, one core:

| change | crawl s |
|---|---|
| as above | 22.4 |
| `Links` read only by the report: an ask a link made carries its page (`LinkedFrom`), and `merge` connects the edge once the ask is no duplicate | 22.3 |
| relation mending (devices/edges.c): sorted anew only once the room taken since the last sort costs about as much as a sort (keys and edges), not after every mending that took room from far (1,356 sorts anew) | 17.7 |
| `classify`: a page is `Html` once, as its answer comes; `settle` no longer lowers the headers of every waiting page each frame | 14.3 |
| `be_polite` by turns: an ask takes the next turn at its host as it comes, and a host lets go as many turns as it has room for (no order of all that wait) | 14.6 |
| a `for ... by` / `in turn by` puts in order only the entities that have and lack its tags as asked (before: every row of its archetypes, asking each in its turn: so `pace`, and `be_polite`'s order of the new asks, sorted all) | 12.3 |
| room after every entity's run, also of those with no edge yet (new pages had none, and took it from ever farther) | 11.0 |

The mock (`bench/http/mock.py`) after them: 10⁴ 0.120 / 0.150 s (one core
/ all cores), 10⁵ 1.752 / 1.926, 10⁶ 22.201 / 22.469 (25.5 / 24.6
before).

The `Links` change alone gained nothing: a relation is kept in order
after every system that connects or despawns, read or not, and the
mending was what cost. Threads now sleep at once in a program on the
http device, as in one with a window (tools/ent), so ent-par's CPU is
ent's.

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.021 | 0.125 | 18 | 10,310 |
| 10⁴ | 64 | ent | 1.096 | 0.282 | 66 | 9,604 |
| 10⁴ | 64 | ent-par | 1.125 | 0.339 | 68 | 9,358 |
| 10⁴ | 256 | C | 1.050 | 0.143 | 24 | 10,027 |
| 10⁴ | 256 | ent | 1.123 | 0.263 | 67 | 9,376 |
| 10⁴ | 256 | ent-par | 1.126 | 0.296 | 66 | 9,353 |
| 10⁴ | 1024 | C | 1.085 | 0.197 | 24 | 9,703 |
| 10⁴ | 1024 | ent | 1.120 | 0.307 | 68 | 9,397 |
| 10⁴ | 1024 | ent-par | 1.156 | 0.334 | 69 | 9,109 |
| 10⁵ | 64 | C | 2.286 | 1.179 | 75 | 46,142 |
| 10⁵ | 64 | ent | 10.636 | 9.705 | 484 | 9,918 |
| 10⁵ | 64 | ent-par | 11.072 | 11.206 | 491 | 9,527 |
| 10⁵ | 256 | C | 2.100 | 1.374 | 75 | 50,230 |
| 10⁵ | 256 | ent | 5.738 | 5.148 | 441 | 18,383 |
| 10⁵ | 256 | ent-par | 6.466 | 6.227 | 447 | 16,315 |
| 10⁵ | 1024 | C | 2.979 | 2.265 | 76 | 35,413 |
| 10⁵ | 1024 | ent | 4.827 | 4.317 | 442 | 21,856 |
| 10⁵ | 1024 | ent-par | 5.258 | 4.721 | 444 | 20,061 |

Left of the 11 s (64 connections, 5,190 frames): `receive` 2.4 s (curl
and waiting), keeping `Links` 1.1 s, the hash indexes `Kept` and `ByUrl`
1.8 s, `extract_links` 0.9 s (the pages waiting for their level,
looked over each frame), the `on added` loops of `history` and `log`
0.8 s, and some 20 loops of 0.1 to 0.2 ms a frame each. What is left is
mostly a floor, not one system: a loop over the asks costs about
0.15 ms a frame at 100,000 of them even where it does nothing for any
of them (a tag not `apart` is a byte each entity has, and the loop asks
each), as do an `on added` loop and a hash index's mending. C does
nothing for an ask until it is its turn. To get there each loop should
cost what changed in the frame, not how many asks there are: tags
`apart` with small capacities for the states few asks are in (in
flight, answered and not settled; `Handled apart` alone gained 0.7 s and
cost 170 MB: an archetype of full capacity more), a log of what was
added for `on added` loops and for the hash indexes, and the pages
waiting for their level held apart until it comes.
