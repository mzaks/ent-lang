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

### Live, after `on added` and hash indexes by what changed

Two changes in the compiler, both for loops that cost by how many asks
there are, not by what changed:

- A reactive `for` (`on added Got`) that gives rows to a buffer,
  connects or changes which entities there are went through every
  entity, so that what it does lands in the order of the rows. It now
  takes the rows the log names, puts them in order and runs its body
  for them so (docs/syntax.md, the reactive `for`): `history` and `log`
  0.8 s to 0.05 s.
- A hash index kept as fits looked at every entity each time it was
  read after a change (were its key and copies still its row's?), and
  where entities came, went or moved, it gave every one its place
  again. Now a reactive `for` lists those that came or changed their
  key, each gets a row at the end of its key's list, and the rows of
  those that went or moved stay till it is made anew (at twice as many
  rows as made); `get` and the `for`s it answers pass over them
  (docs/indexes.md, 5.1): `Kept` and `ByUrl` 1.8 s to 0.12 s.

Traced at 10⁵ pages and 64 connections (one core, 5,259 frames): 8.7 s,
of which `receive` 2.4 s, `Links` kept in order 1.3 s, `extract_links`
1.0 s, and the rest loops over every ask (0.02 to 0.06 ms a frame
each). `extract_links` is taking the links out of the pages, not
looking over those waiting for their level: its median frame is 1 µs,
and nine frames in ten come to 84 ms in all. Holding the waiting pages
apart (an `apart` tag till the level comes to them) was tried: 20 ms
less, 110 MB more; not kept.

The mock (few frames: 123 at 10⁵) is as it was: 10⁴ 0.112 / 0.146 s
(one core / all cores), 10⁵ 1.755 / 1.939, 10⁶ 22.846 / 24.327 (22.2 /
22.5 before). Live:

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.009 | 0.119 | 23 | 10,438 |
| 10⁴ | 64 | ent | 1.101 | 0.239 | 42 | 9,565 |
| 10⁴ | 64 | ent-par | 1.122 | 0.320 | 50 | 9,384 |
| 10⁴ | 256 | C | 1.045 | 0.134 | 23 | 10,073 |
| 10⁴ | 256 | ent | 1.123 | 0.232 | 47 | 9,371 |
| 10⁴ | 256 | ent-par | 1.123 | 0.277 | 61 | 9,375 |
| 10⁴ | 1024 | C | 1.076 | 0.214 | 23 | 9,782 |
| 10⁴ | 1024 | ent | 1.162 | 0.324 | 65 | 9,064 |
| 10⁴ | 1024 | ent-par | 1.171 | 0.323 | 61 | 8,989 |
| 10⁵ | 64 | C | 2.319 | 1.200 | 75 | 45,484 |
| 10⁵ | 64 | ent | 7.421 | 6.328 | 340 | 14,215 |
| 10⁵ | 64 | ent-par | 8.011 | 7.233 | 340 | 13,169 |
| 10⁵ | 256 | C | 2.152 | 1.398 | 75 | 49,018 |
| 10⁵ | 256 | ent | 4.929 | 4.270 | 306 | 21,402 |
| 10⁵ | 256 | ent-par | 5.456 | 4.924 | 315 | 19,334 |
| 10⁵ | 1024 | C | 3.086 | 2.364 | 76 | 34,180 |
| 10⁵ | 1024 | ent | 4.747 | 4.129 | 309 | 22,220 |
| 10⁵ | 1024 | ent-par | 5.101 | 4.469 | 315 | 20,681 |

Peak memory fell by a third (484 to 340 MB at 64 connections; which
part, not looked into). ent-par is slower than ent here, as before; the
loops that now go through the logs in row order run on one thread.

### Live, with the states few asks are in kept apart

The loops left went over every ask each frame to find the few in one
state. Now:

- **Going** (`tag Going apart`, pipeline.ent): let go and not answered
  yet. `ready` gives it as an ask comes, is let go or is to be tried
  again; the device sends those without `Sent` and takes it away with
  the answer. `send`, `fetch`, `time_out`, `warn_slow`, `pace` and the
  hosts' counts go over just those (at most the hosts' connections: a
  capacity of 4,096; the mock, which sends all at once, says more).
  One tag for both states: an entity given one `apart` tag and losing
  another in one `for` is moved once, by the last (open).
- **Once per event**: `retry`, `follow_redirect`, `settle`,
  `number_asks`, `drop_duplicates`, `merge`, `forget_duplicates`,
  `inherit_depth`, `take_depth`, `place` and the mock's `fill` are
  `on added` / `on changed` loops, which go through the logs. `place`
  looks at an ask again (`Recheck`) where it waited for a robots.txt
  and one came.
- In the compiler, for these: a reactive `for` may keep a running sum
  (`number_asks`), and one that sends whole numbers to a field it also
  reads (`merge`: `Depth(t.of).d min= d.d`) goes through the logs, those
  waiting on lists till it ends; both visit the rows in order, as a scan
  does. And a connect given where it stands no longer has its edge
  placed again by the scan of a first run (it stopped the program).

Traced at 10⁵ pages and 64 connections: 8.7 to 5.5 s; measured, 7.4 to
5.0 s (C 2.3), at 256 connections 4.9 to 4.2 s, at 1024 4.7 to 4.5. Left: `receive`
2.0 s, `Links` kept in order 1.2 s (sorted after each of the four
systems that connect or despawn, 24,050 times, read only by the report),
`extract_links` 0.8 s (taking the links out), and `be_polite` letting
asks go 0.47 s: each held ask looks at its host's turn every frame (a
host's queue is what C has).

The mock (few frames, most asks changing in each) is as it was, a
little slower at 10⁵: 10⁴ 0.118 / 0.152 s (one core / all cores), 10⁵
1.809 / 1.921, 10⁶ 22.499 / 22.115 (0.112 / 0.146, 1.755 / 1.939,
22.846 / 24.327 before). Live:

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.010 | 0.124 | 23 | 10,422 |
| 10⁴ | 64 | ent | 1.097 | 0.233 | 41 | 9,595 |
| 10⁴ | 64 | ent-par | 1.092 | 0.314 | 41 | 9,644 |
| 10⁴ | 256 | C | 1.047 | 0.137 | 23 | 10,059 |
| 10⁴ | 256 | ent | 1.099 | 0.229 | 40 | 9,575 |
| 10⁴ | 256 | ent-par | 1.116 | 0.285 | 42 | 9,437 |
| 10⁴ | 1024 | C | 1.069 | 0.202 | 23 | 9,846 |
| 10⁴ | 1024 | ent | 1.116 | 0.295 | 42 | 9,436 |
| 10⁴ | 1024 | ent-par | 1.102 | 0.337 | 43 | 9,550 |
| 10⁵ | 64 | C | 2.285 | 1.181 | 69 | 46,171 |
| 10⁵ | 64 | ent | 5.041 | 4.015 | 312 | 20,927 |
| 10⁵ | 64 | ent-par | 5.495 | 4.665 | 314 | 19,197 |
| 10⁵ | 256 | C | 2.078 | 1.363 | 76 | 50,757 |
| 10⁵ | 256 | ent | 4.233 | 3.624 | 269 | 24,918 |
| 10⁵ | 256 | ent-par | 4.539 | 3.911 | 271 | 23,243 |
| 10⁵ | 1024 | C | 3.001 | 2.300 | 76 | 35,147 |
| 10⁵ | 1024 | ent | 4.545 | 3.863 | 270 | 23,211 |
| 10⁵ | 1024 | ent-par | 4.773 | 4.127 | 273 | 22,103 |

Memory: each kind of ask that can be `Going` has an archetype for it
as well. The robots.txt asks, one a host, now have an archetype of
1,024 (`tag RobotsAsk capacity 1024`), where they had one as big as
every other kind's: so the live crawl holds less than before (312 MB
where 340). The mock lets every ask go at once: a third of the pages
are going at its most (36,207 at 10⁵, 360,260 at 10⁶), and it gives
`Going` half the asks' capacity; 589 MB at 10⁵ where 565.

### Live, Links sorted once and a queue for each host

- **`Links` sorted once.** Nothing reads it but the report, a `for`
  along its edges; it was put in order after every system that connects
  (23,202 times, 0.79 s at 10⁵ pages and 64 connections). The compiler
  now sorts a relation that only such `for`s read before them, not
  after each change (docs/syntax.md, "Relations that change"): once, at
  the report, 1.5 ms. A connect that finds it full sorts it first,
  which drops the edges of entities gone.
- **A queue for each host.** `take_turns` gives each ask, as it comes,
  its host's next turn and a key, the host's number above the turn;
  `ByTurn` is a hash index of those keys. `be_polite` finds the asks
  whose turn came by it (a buffer of them, `LetGo`, then `for .. in
  LetGo`), where every held ask looked at its host's turn every frame:
  0.56 to 0.02 s. (`take_turns` still looked at every ask with a host
  for those without a turn, 0.15 s: the asks are one family of
  archetypes where `Turn` is held optionally. A `for` by a field may now
  react, and `take_turns` is `on added At`, going over the asks given a
  host since: 0.15 s to 9 ms, see below.)

Traced at 10⁵ pages and 64 connections: 5.1 to 3.7 s; `receive` 2.1 s
and `extract_links` 0.7 s are most of what is left. Measured (C in the
same run):

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.060 | 0.251 | 24 | 9,928 |
| 10⁴ | 64 | ent | 1.176 | 0.361 | 70 | 8,954 |
| 10⁴ | 64 | ent-par | 1.223 | 0.515 | 71 | 8,605 |
| 10⁴ | 256 | C | 1.086 | 0.271 | 24 | 9,691 |
| 10⁴ | 256 | ent | 1.202 | 0.391 | 70 | 8,761 |
| 10⁴ | 256 | ent-par | 1.253 | 0.510 | 71 | 8,401 |
| 10⁴ | 1024 | C | 1.188 | 0.376 | 24 | 8,861 |
| 10⁴ | 1024 | ent | 1.248 | 0.499 | 71 | 8,435 |
| 10⁴ | 1024 | ent-par | 1.314 | 0.587 | 73 | 8,011 |
| 10⁵ | 64 | C | 2.415 | 1.288 | 72 | 43,684 |
| 10⁵ | 64 | ent | 3.703 | 2.573 | 478 | 28,487 |
| 10⁵ | 64 | ent-par | 4.144 | 3.301 | 479 | 25,454 |
| 10⁵ | 256 | C | 2.257 | 1.517 | 75 | 46,730 |
| 10⁵ | 256 | ent | 3.170 | 2.458 | 434 | 33,278 |
| 10⁵ | 256 | ent-par | 3.460 | 2.835 | 431 | 30,487 |
| 10⁵ | 1024 | C | 3.194 | 2.482 | 75 | 33,031 |
| 10⁵ | 1024 | ent | 4.100 | 3.420 | 445 | 25,727 |
| 10⁵ | 1024 | ent-par | 4.428 | 3.769 | 439 | 23,820 |

Peak memory is not to be compared with the tables before: transparent
huge pages are on (`always`), and what the arena's pages come to
depends on how much memory is free. The program before these two
changes, measured the same hour: 441 MB at 10⁵ and 64 connections; `Links`
sorted once 434, with the queue (its index and buffer) 474.

The mock (few frames) is as it was: 10⁴ 0.113 / 0.164 s (one core / all
cores), 10⁵ 1.754 / 1.927, 10⁶ 22.937 / 22.555. It lets every ask go at
once, so it gives `LetGo` half the asks' capacity, as `Going`.

### Live, `take_turns` on added

`take_turns` goes over the asks given a host since the frame before (a
`for` by a field that reacts, `on added At`), not every ask with a host:
traced at 10⁵ pages and 64 connections, 0.15 s to 9 ms; the whole crawl
3.37 s traced, `receive` 1.99 s and `extract_links` 0.66 s of it.
Measured (C in the same run):

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.010 | 0.127 | 23 | 10,419 |
| 10⁴ | 64 | ent | 1.087 | 0.203 | 72 | 9,687 |
| 10⁴ | 64 | ent-par | 1.093 | 0.289 | 72 | 9,633 |
| 10⁴ | 256 | C | 1.048 | 0.145 | 23 | 10,049 |
| 10⁴ | 256 | ent | 1.065 | 0.194 | 71 | 9,888 |
| 10⁴ | 256 | ent-par | 1.111 | 0.265 | 74 | 9,479 |
| 10⁴ | 1024 | C | 1.087 | 0.208 | 23 | 9,688 |
| 10⁴ | 1024 | ent | 1.129 | 0.286 | 71 | 9,326 |
| 10⁴ | 1024 | ent-par | 1.134 | 0.317 | 73 | 9,287 |
| 10⁵ | 64 | C | 2.252 | 1.155 | 75 | 46,836 |
| 10⁵ | 64 | ent | 3.334 | 2.238 | 480 | 31,639 |
| 10⁵ | 64 | ent-par | 3.718 | 2.870 | 485 | 28,374 |
| 10⁵ | 256 | C | 2.029 | 1.318 | 76 | 51,983 |
| 10⁵ | 256 | ent | 2.875 | 2.135 | 442 | 36,687 |
| 10⁵ | 256 | ent-par | 3.082 | 2.457 | 438 | 34,221 |
| 10⁵ | 1024 | C | 2.899 | 2.207 | 76 | 36,391 |
| 10⁵ | 1024 | ent | 3.759 | 3.040 | 439 | 28,059 |
| 10⁵ | 1024 | ent-par | 3.958 | 3.355 | 437 | 26,650 |

At 10⁴ ent is within 3 to 8% of C; at 10⁵ it takes 1.3 to 1.5 times
as long, most of the rest being `receive`.

### Texts made cut from arenas

`receive` is the device and the network, as much as C's: profiled at
10⁵ pages and 64 connections, curl takes 0.51 s of ent's user time and
0.48 s of C's, and both wait 1.1 s. What C did not spend was text: each
text made while a schedule ran (a slice, a join, a body read) had a
malloc of its own, two atomic adds and a push onto a list, and the list
was walked and freed when the schedule was done; `ent_text_make` and
`ent_text_release` with malloc and free were a fifth of the user time.
Now a thread cuts them from rooms of its own, one after the other, and
they are given back all at once (devices/text.c; a partitioned run on
ranks keeps its lists). User time 1.73 to 1.36 s. (`allowed`, which
joined the allow list for each link, is a `HostByName` lookup now, as C
has it; within noise once joining is cheap.)

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.020 | 0.133 | 24 | 10,321 |
| 10⁴ | 64 | ent | 1.049 | 0.176 | 63 | 10,033 |
| 10⁴ | 64 | ent-par | 1.059 | 0.232 | 62 | 9,939 |
| 10⁴ | 256 | C | 1.046 | 0.146 | 24 | 10,069 |
| 10⁴ | 256 | ent | 1.064 | 0.201 | 64 | 9,897 |
| 10⁴ | 256 | ent-par | 1.074 | 0.220 | 63 | 9,801 |
| 10⁴ | 1024 | C | 1.076 | 0.213 | 24 | 9,784 |
| 10⁴ | 1024 | ent | 1.091 | 0.262 | 68 | 9,650 |
| 10⁴ | 1024 | ent-par | 1.070 | 0.268 | 68 | 9,843 |
| 10⁵ | 64 | C | 2.246 | 1.147 | 70 | 46,962 |
| 10⁵ | 64 | ent | 2.879 | 1.777 | 374 | 36,640 |
| 10⁵ | 64 | ent-par | 3.289 | 2.415 | 379 | 32,069 |
| 10⁵ | 256 | C | 2.043 | 1.334 | 76 | 51,639 |
| 10⁵ | 256 | ent | 2.337 | 1.628 | 350 | 45,141 |
| 10⁵ | 256 | ent-par | 2.450 | 1.812 | 351 | 43,050 |
| 10⁵ | 1024 | C | 2.863 | 2.160 | 76 | 36,843 |
| 10⁵ | 1024 | ent | 3.132 | 2.416 | 354 | 33,682 |
| 10⁵ | 1024 | ent-par | 3.216 | 2.551 | 346 | 32,804 |

At 10⁵ ent takes 1.09 to 1.28 times as long as C (1.3 to 1.5 before),
and holds less at its peak (374 MB where 480, the same hour's runs).
The mock, which makes far more texts a frame, gains most: 10⁴ 0.113 to
0.058 s, 10⁵ 1.754 to 0.684 s on one core (0.068 / 0.704 on all).

### Pages due, not looked over

`extract_links` went over every ask each frame that something might be
due, for the few with `Html` and without `Handled` (9% of the user time
at 10⁵ pages and 64 connections, 0.12 s). Now `due_pages` marks a page
`Due` as it gets `Html` or a lower depth where its level is there, and
looks the waiting ones over only when `Level` changes (12 times in that
crawl); `extract_links` runs `on added Due`. That needed the compiler:
a reactive `for` that sends whole numbers (`min=`) and connects from a
loop of its body is now walked in the order of the rows, as one that
does so outside a loop was (docs/syntax.md). What is left of
`extract_links` is the links themselves: `next_link` 0.13 s, `Kept.get`
0.05 s (C: 0.17 s in all).

| pages | connections | crawler | wall s | CPU s | peak MB | pages/s |
|---|---|---|---|---|---|---|
| 10⁴ | 64 | C | 1.011 | 0.126 | 24 | 10,411 |
| 10⁴ | 64 | ent | 1.046 | 0.162 | 63 | 10,066 |
| 10⁴ | 64 | ent-par | 1.041 | 0.169 | 65 | 10,118 |
| 10⁴ | 256 | C | 1.041 | 0.147 | 24 | 10,117 |
| 10⁴ | 256 | ent | 1.052 | 0.196 | 62 | 10,010 |
| 10⁴ | 256 | ent-par | 1.051 | 0.196 | 65 | 10,013 |
| 10⁴ | 1024 | C | 1.085 | 0.214 | 24 | 9,701 |
| 10⁴ | 1024 | ent | 1.084 | 0.260 | 70 | 9,715 |
| 10⁴ | 1024 | ent-par | 1.087 | 0.258 | 71 | 9,689 |
| 10⁵ | 64 | C | 2.252 | 1.165 | 76 | 46,848 |
| 10⁵ | 64 | ent | 2.783 | 1.652 | 378 | 37,903 |
| 10⁵ | 64 | ent-par | 2.795 | 1.695 | 377 | 37,746 |
| 10⁵ | 256 | C | 2.032 | 1.327 | 76 | 51,911 |
| 10⁵ | 256 | ent | 2.322 | 1.595 | 352 | 45,422 |
| 10⁵ | 256 | ent-par | 2.314 | 1.627 | 359 | 45,593 |
| 10⁵ | 1024 | C | 2.864 | 2.178 | 76 | 36,835 |
| 10⁵ | 1024 | ent | 3.129 | 2.392 | 356 | 33,716 |
| 10⁵ | 1024 | ent-par | 3.115 | 2.440 | 361 | 33,869 |

At 64 connections 2.88 to 2.78 s on one core, and 3.29 to 2.80 s on
all: the parallel build no longer looks the asks over on its threads
each frame. At 256 and 1024 as before. The mock as before: 10⁴ 0.059 /
0.073 s, 10⁵ 0.688 / 0.683.

### Links read as a page comes

At a level's end every page of the next level was due at once, and
`extract_links` scanned them all in one frame: at 10⁶ pages 2.15 s,
over the 2 s an ask is given (curl is not driven meanwhile), so 64
asks answered were said to have timed out and asked again (C4, below).
Now `read_links` scans a page as it comes, keeps the URLs to allowed
hosts in `Html` (a line each: `var links = ""` grows, which needed the
compiler) and lets the body go; at the level's end `extract_links` only
makes the asks and edges. By `--trace` at 10⁶ pages, the longest frame
is 1.21 s, `extract_links` 0.75 s of it and ByUrl's index growing
0.22 s.

| pages | crawler | asks | wall s | CPU s | peak MB |
|---|---|---|---|---|---|
| 10⁴ | ent, before | 10,669 | 1.051 | 0.177 | 62 |
| 10⁴ | ent | 10,669 | 1.042 | 0.167 | 59 |
| 10⁵ | ent, before | 106,964 | 2.783 | 1.644 | 375 |
| 10⁵ | ent | 106,964 | 2.802 | 1.657 | 343 |
| 10⁶ | ent, before | 1,069,255 | 36.517 | 22.817 | 2,825 |
| 10⁶ | ent | 1,069,191 | 36.656 | 22.965 | 2,416 |
| 10⁶ | C | 1,069,191 | 27.775 | 15.920 | 623 |

The asks are C's again, and the bodies waiting for their level are not
kept (15% less memory at 10⁶); the time is as it was (the frame was
work done anyway, in one place). The mock at 10⁵: 0.70 to 0.75 s on
one core (the URLs joined, then cut again), 0.67 to 0.57 s on all
(pages are read on many threads).

### The open asks counted as they change

`advance_level` counted the open asks over every ask not settled, each
frame: at 10⁶ pages, 50,000 frames, 2.7 s of the crawl (`--trace`). Now
it counts as they change: the asks kept (`on added Known`) and settled
(`on added Handled`), and those not settled at the level or less by a
flag of each ask's `Depth` (set when it is kept, gets a depth or a
lower one, or the level comes to it; cleared when it is settled), a
sweep only when the level moves. Checked first against the counts it
replaces at every frame (crawl checks, the mock at 10⁵, live at 10⁶):
the same. `advance_level` is 0.12 s at 10⁶.

| pages | crawler | wall s | CPU s | peak MB |
|---|---|---|---|---|
| 10⁴ | ent, before | 1.042 | 0.167 | 59 |
| 10⁴ | ent | 1.045 | 0.173 | 62 |
| 10⁵ | ent, before | 2.802 | 1.657 | 343 |
| 10⁵ | ent | 2.792 | 1.658 | 334 |
| 10⁶ | ent, before | 36.656 | 22.965 | 2,416 |
| 10⁶ | ent | 33.732 | 19.708 | 2,477 |
| 10⁶ | ent-par | 33.528 | 20.105 | 2,511 |
| 10⁶ | C | 27.869 | 16.024 | 622 |

At 10⁶ ent takes 1.21 times C's time (1.32 before); at 10⁵ and 10⁴ the
frames were few and short enough that the count did not show.

## Against other crawlers (§9.2, C4)

`others.py`: wget, Scrapy, Colly and spider crawl the generated site
through serve.c as crawl.c and crawl_live do, each told to do the same
crawl as near as it can be (README.md: versions, setups). 64 requests
out at once (wget: one). Single-threaded crawlers on one core (CPU 2),
the others on four (CPUs 0 to 3); the server on 4 to 11.

**What each asked for**, by the server's log, against crawl.c (a retry
is an ask; URLs: the distinct ones):

| pages | crawler | URLs | asks | not asked | more |
|---|---|---|---|---|---|
| 10⁴ | C | 10,528 | 10,669 | | |
| 10⁴ | ent, ent-par | 10,528 | 10,669 | 0 | 0 |
| 10⁴ | wget | 10,528 | 10,688 | 0 | 0 |
| 10⁴ | Scrapy | 10,528 | 10,669 | 0 | 0 |
| 10⁴ | Colly | 10,528 | 10,855 | 0 | 0 |
| 10⁴ | spider | 10,627 | 10,880 | 98 | 197 |
| 10⁵ | C | 105,487 | 106,964 | | |
| 10⁵ | ent, ent-par | 105,487 | 106,964 | 0 | 0 |
| 10⁵ | wget | 105,487 | 107,089 | 0 | 0 |
| 10⁵ | Scrapy | 105,487 | 106,964 | 0 | 0 |
| 10⁵ | Colly | 105,487 | 107,808 | 0 | 0 |
| 10⁵ | spider | 106,406 | 108,868 | 1,005 | 1,924 |
| 10⁶ | C | 1,054,531 | 1,069,191 | | |
| 10⁶ | ent, ent-par | 1,054,531 | 1,069,255 | 0 | 0 |
| 10⁶ | Colly (killed at 14 GB) | 621,793 | 635,222 | 432,738 | 0 |
| 10⁶ | spider, in memory | 1,064,055 | 1,088,288 | 9,978 | 19,502 |

- **Scrapy** asks exactly what C does, once its spider looks at a link's
  host without the port: `allow_domains` takes `site.test:80` for
  another host, and every page only linked that way was lost.
- **wget** asks some pages twice: those it reached by a redirect and
  by a link (0.1%).
- **Colly** asks a hop of a redirect chain again where a page links to
  it: it follows redirects inside Go's HTTP client, which its dedupe
  does not see (1 to 3% more asks, from run to run: which hop a page
reaches first races). Its setup puts URLs in their normal
  form before visiting them (Colly knows `HTTP://SITE.TEST:80/x#f` and
  `http://site.test/x` as two), visits one at a time (it looks whether a
  URL was visited and marks it in two steps, and async pages raced:
  2,400 asks twice), and counts retries by URL (a request a page visits
  shares the page's context).
- **spider** resolves the relative links of a page it reached by a
  redirect against the URL it asked for, not where the redirect ended
  (the 404s more: `/r/61/9664.html` for `/p/9664.html`), and leaves
  most images out though told to take resources. Its own retry backoff
  (a second, doubling up to a minute) took a crawl of 10⁴ pages from
  1.5 s to 18 s; its setup retries after 10 ms as the others do. It
  still retries the redirect chains over the limit, half a second
  apart (2.5 s of its wall time at either size).

**Measured** (median of 3; URLs/s: the distinct URLs it asked for).
Peak memory is the crawler's own: `others.py` starts each from a process
forked while it is small. (Measured first from the script itself, after
it had read the logs, every peak was at least what the script held,
some 110 MB at 10⁵: a child's peak starts at what it was forked from,
and exec keeps it.)

| pages | crawler | wall s | CPU s | peak MB | URLs/s |
|---|---|---|---|---|---|
| 10⁴ | C | 1.025 | 0.127 | 15 | 10,274 |
| 10⁴ | ent | 1.051 | 0.177 | 62 | 10,021 |
| 10⁴ | ent-par | 1.038 | 0.173 | 65 | 10,146 |
| 10⁴ | wget | 8.784 | 0.378 | 12 | 1,199 |
| 10⁴ | Scrapy | 12.031 | 11.881 | 100 | 875 |
| 10⁴ | Colly | 0.647 | 1.304 | 225 | 16,271 |
| 10⁴ | spider | 3.255 | 0.362 | 19 | 3,265 |
| 10⁴ | spider, in memory | 3.263 | 0.338 | 18 | 3,256 |
| 10⁵ | C | 2.252 | 1.169 | 69 | 46,851 |
| 10⁵ | ent | 2.783 | 1.644 | 375 | 37,910 |
| 10⁵ | ent-par | 2.773 | 1.687 | 383 | 38,035 |
| 10⁵ | wget | 85.358 | 3.700 | 74 | 1,236 |
| 10⁵ | Scrapy | 155.883 | 155.192 | 196 | 677 |
| 10⁵ | Colly | 6.245 | 13.699 | 2,232 | 16,892 |
| 10⁵ | spider | 284.997 | 286.592 | 438 | 373 |
| 10⁵ | spider, in memory | 4.037 | 2.768 | 32 | 26,358 |
| 10⁶ | C | 27.703 | 15.897 | 622 | 38,066 |
| 10⁶ | ent | 36.517 | 22.817 | 2,825 | 28,878 |
| 10⁶ | ent-par | 36.034 | 23.508 | 2,865 | 29,265 |
| 10⁶ | Colly | killed at 14 GB after 14 s | 50.7 | | |
| 10⁶ | spider, in memory | 23.016 | 36.816 | 233 | 46,232 |

- ent comes after C only at 10⁵: 2.2 times as fast as Colly (on one
  core, Colly on four, with 8 times its CPU time and 6 times its
  memory), 1.5 times spider at its best, 31 times wget and 56 times
  Scrapy. Its memory is the most after Colly's: the asks are kept for
  the report (§12, question 3), with their texts (some 2.7 KB an ask;
  C keeps 0.6).
- At 10⁶ spider (in memory, on four cores) is the fastest, 1.2 times C
  and 1.6 times ent, in 233 MB; it still leaves out 1% (images) and
  asks 2% more (404s). C and ent take 12 and 13 times as long as at
  10⁵, spider 6 times (its 3 s of redirect retries are the same at any
  size). Colly does not finish: its memory reaches the 14 GB the run
  was given (`MemoryMax`) after 620,000 URLs.
- ent at 10⁶ asks 64 pages twice. When a level is settled, the links
  of all the next level's pages are taken in one frame (2.15 s at
  27.6 s, `extract_links` 1.63 s and ByUrl's index growing 0.23 s, by
  `--trace`); curl is not driven meanwhile, and the asks sent at that
  frame's start are over their 2 s when it is: curl says they timed out
  though their answers wait, and they are asked again. It is the only
  frame over 0.75 s. (Since, the links are read as a page comes, above:
  the frame is 1.21 s, and the asks are C's.)
- Colly at 10⁴ is faster than C and ent: they take a page's links only
  once every ask less deep is settled (so that each URL's depth is its
  least), and a slow page holds its level; Colly does not wait.
- Scrapy is Python bound (its CPU time is its wall time); wget waits
  on its one connection.
- spider keeps at most 15,000 URLs it visited in memory, by default,
  and the rest in SQLite on disk (`LINKS_VISITED_MEMORY_LIMIT`): past
  that a crawl of 10⁵ pages takes 280 s of CPU. "In memory" raises the
  limit.

- wget ends with 4 (a network failure): the site's pages that never
  answer.

Not measured at 10⁶: wget and Scrapy (some 15 and 25 minutes a run,
and 30 and 55 times slower at 10⁵ already) and spider by default (its
SQLite took 280 s at 10⁵). Not yet: the second site, a mirror of a
public practice site (§9.3), which is to be fetched once by hand.
