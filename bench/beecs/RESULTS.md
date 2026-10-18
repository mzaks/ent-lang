# The honeybee colony: timings

`examples/beecs` against beecs (Go 1.26.1, commit 28958a0, built by
`examples/beecs/reference/fetch.sh`), on the Ryzen AI 9 HX 370.

## 2026-10-14, first

`python3 bench/beecs/run.py --seeds 9 --parallel`: a year of beecs's
defaults (two patches, Berlin's weather of 2000), with the squadrons of
foragers 100 bees (the default: some 100 to 200 entities), 10 and 1 (each
forager an entity, 10,000 to 20,000). The median of 9 seeds, a process
each, its whole time (setting up, reading the weather, writing the days);
one core is CPU 2, the model built `-march=native`.

| squadron size | | a year | memory |
|---|---|---|---|
| 100 | beecs (Go), one core | 23.3 ms | 17 MB |
|  | model, one core | 23.0 ms | 23 MB |
|  | model, all cores | 25.4 ms | 27 MB |
| 10 | beecs (Go), one core | 163.2 ms | 17 MB |
|  | model, one core | 147.0 ms | 23 MB |
|  | model, all cores | 154.7 ms | 27 MB |
| 1 | beecs (Go), one core | 1,553.0 ms | 17 MB |
|  | model, one core | 1,407.9 ms | 23 MB |
|  | model, all cores | 1,539.7 ms | 29 MB |

All the cores do not help: a round of foraging is a dozen small systems,
four of which go through the squadrons in an order (`in turn by`) on one
core, and there are some ten rounds a day.

How it got here, with squadrons of one:

- First: killed after two minutes. A `for ... in turn by` put its
  entities in order by putting each where it goes, as many steps as
  entities for each of them where they came in an order far from the
  one asked for, as numbers at random do: three such `for`s a round of
  20,000 squadrons.
- 7.3 s: the compiler lists them as they are stored and sorts the list
  once (a merge sort; still one look over them where they are in order).
- 1.83 s: a radix sort instead, a pass for each byte in which the keys
  differ.
- 1.65 s: each squadron's random numbers from the round's key with one
  stir, not four (`key_of` in random.ent).
- 1.40 s: places in an order at random of 32 bits (four passes of the
  sort, not eight), and the squadrons that are not to be visited given
  one place after all the others, so they are gone through as stored.

## 2026-10-15, without turns

`python3 bench/beecs/run.py --seeds 9 --parallel --claims`: the same, and
the model without turns (`Variant.Claims true`, examples/beecs/claims.ent).

| squadron size | | a year | memory |
|---|---|---|---|
| 100 | beecs (Go), one core | 24.0 ms | 17 MB |
|  | model, one core | 23.2 ms | 23 MB |
|  | without turns, one core | 16.9 ms | 27 MB |
|  | model, all cores | 35.6 ms | 29 MB |
|  | without turns, all cores | 23.3 ms | 37 MB |
| 10 | beecs (Go), one core | 159.6 ms | 17 MB |
|  | model, one core | 146.6 ms | 23 MB |
|  | without turns, one core | 121.0 ms | 27 MB |
|  | model, all cores | 155.2 ms | 29 MB |
|  | without turns, all cores | 143.6 ms | 37 MB |
| 1 | beecs (Go), one core | 1,513.3 ms | 17 MB |
|  | model, one core | 1,386.0 ms | 24 MB |
|  | without turns, one core | 1,166.7 ms | 27 MB |
|  | model, all cores | 1,545.4 ms | 31 MB |
|  | without turns, all cores | 1,375.9 ms | 39 MB |

With squadrons of one, the steps that took turns took some 700 ms of the
year (recruits 97, experienced squadrons 164, lining up the resting 159,
dancing 276); without turns some 500 (asking 59, sharing out and flying
93, the lists of dancers and resting 197, the dances drawn 74, told 28,
followed 49). The rest is as it was: deciding 310, the flights' cost 221.

All the cores still gain nothing. A year has some 3,700 rounds over some
8,500 squadrons on average: a loop of a round is 10 to 60 us of work, which
is about what sharing it out among 24 threads costs, and most loops stay
on one core anyway (a `for` is run in parallel from a million entities,
weighed by its calls out). Built with that threshold at 2,048 to 32,768
(by hand), the year with turns took 1.73 to 2.05 s on all cores.
A colony ten times as large (100,000 bees, squadrons of one, 180 days,
some 46,000 squadrons): one core 0.70 s, all cores 0.75 to 0.81; with the
threshold at 4,096 and 8 threads 0.56 to 0.68, the machine not quiet.

## 2026-10-15, profiled

`python3 bench/beecs/run.py --seeds 9 --parallel --claims` after the
changes below.

| squadron size | | a year | memory |
|---|---|---|---|
| 100 | beecs (Go), one core | 23.8 ms | 17 MB |
|  | model, one core | 24.4 ms | 19 MB |
|  | without turns, one core | 11.3 ms | 21 MB |
|  | model, all cores | 25.4 ms | 23 MB |
|  | without turns, all cores | 21.3 ms | 29 MB |
| 10 | beecs (Go), one core | 159.2 ms | 17 MB |
|  | model, one core | 115.8 ms | 19 MB |
|  | without turns, one core | 68.7 ms | 21 MB |
|  | model, all cores | 132.1 ms | 23 MB |
|  | without turns, all cores | 89.2 ms | 29 MB |
| 1 | beecs (Go), one core | 1,497.6 ms | 17 MB |
|  | model, one core | 1,082.9 ms | 19 MB |
|  | without turns, one core | 635.7 ms | 21 MB |
|  | model, all cores | 1,322.2 ms | 25 MB |
|  | without turns, all cores | 886.3 ms | 30 MB |

Profiled with squadrons of one, without turns: the time of each step (the
machine's clock between steps), the processor's counters (cycles,
instructions, branches missed) and samples of where it is (perf_event_open,
by hand: there is no perf here). A year is some 3,700 rounds over 8,500
squadrons on average, 72% of them resting; their columns are a few hundred
kilobytes, so what costs is the instructions for each squadron and the
branches on its state and on numbers at random, about a fifth of the
cycles missed. Step by step, from 1,157 ms:

- 1,086: numbers at random drawn only where asked for (`decide` drew one
  for starting for every squadron, and both sides of `&&`; the flight's
  mortality one for every squadron, also those that rest); the chance to
  give up a pollen patch (a `pow`) and a dance's chance to lead none (an
  `exp`) once for each patch. The same numbers: the days to the bit.
- 922: `flight_cost` sent the honey and the time from two places each.
  Values sent to a field from more than one place are kept for every row
  (eight marks cleared for every squadron) and added up after the `for`
  in a second pass, one after another; from one place they are added as
  the `for` goes.
- 875: the numbers every squadron draws in `decide`, for pollen and for
  starting, in a loop of their own, which the compiler does eight at a
  time (AVX-512); the same numbers.
- 859: the dancers given rows from one place, not two.
- 795: the followers drawn by a shuffle that stops when they are all
  drawn, not a number for every one resting.
- 636: what follows the flights in two passes over the squadrons, not
  seven (`fly`: getting a load, its cost, dying on the way, the dancers
  and the resting; `come_home`: following and unloading).

Then 5.7 billion instructions where there were 9.9, at 2.15 an instruction
a cycle, 4.5% of the branches missed. `decide` 35%, `fly` 33%, `come_home`
12%, `claims` 9%, `dances` 6%.

What the compiler could do better, seen here:

- `&&` and `||` work out both sides: a draw on the right is drawn for every
  entity. Going to the right only where it counts, as C does, would let
  these be written as they read. (Done since: see docs/syntax.md.)
- A `for` that gives a buffer rows marks every row, and puts them together
  after the `for` with a branch on each mark: with 72% of the squadrons
  resting that branch is missed often (the most missed in `fly`).
- Values sent from more than one place in a `for` are kept for every row
  and added after it in a second pass.
- The rows of a buffer given from two places come grouped by place, not
  in the order of the entities as docs/syntax.md says.
- `fuse-systems` (one loop for several systems) is only built with
  --parallel, and does not take systems that send or give rows; by hand it
  was the largest step here. (Built on one core too since; it still does
  not take such systems.)

## 2026-10-15, by profile

`python3 bench/beecs/run.py --seeds 9 --parallel --claims --pgo`: as
before, and each build also built by its profile (`tools/ent --instrument`,
a year of each squadron size and each way of foraging for seeds 101 and
102, then `--profile`). The compiler as of then: systems fused on one
core too (also those that send, look up or write uniques) and the
program's functions internal.

| squadron size | | a year | memory |
|---|---|---|---|
| 100 | beecs (Go), one core | 23.5 ms | 17 MB |
|  | model, one core | 19.6 ms | 19 MB |
|  | without turns, one core | 10.7 ms | 21 MB |
|  | model, one core, PGO | 19.8 ms | 19 MB |
|  | without turns, one core, PGO | 10.4 ms | 21 MB |
|  | model, all cores | 23.1 ms | 23 MB |
|  | without turns, all cores | 15.1 ms | 25 MB |
|  | model, all cores, PGO | 26.5 ms | 23 MB |
|  | without turns, all cores, PGO | 15.7 ms | 25 MB |
| 10 | beecs (Go), one core | 162.9 ms | 17 MB |
|  | model, one core | 117.0 ms | 19 MB |
|  | without turns, one core | 68.6 ms | 21 MB |
|  | model, one core, PGO | 114.9 ms | 19 MB |
|  | without turns, one core, PGO | 69.7 ms | 21 MB |
|  | model, all cores | 124.7 ms | 23 MB |
|  | without turns, all cores | 80.9 ms | 25 MB |
|  | model, all cores, PGO | 125.1 ms | 23 MB |
|  | without turns, all cores, PGO | 77.9 ms | 25 MB |
| 1 | beecs (Go), one core | 1,523.9 ms | 17 MB |
|  | model, one core | 1,092.3 ms | 19 MB |
|  | without turns, one core | 636.1 ms | 21 MB |
|  | model, one core, PGO | 1,081.0 ms | 19 MB |
|  | without turns, one core, PGO | 639.1 ms | 21 MB |
|  | model, all cores | 1,259.9 ms | 25 MB |
|  | without turns, all cores | 713.5 ms | 27 MB |
|  | model, all cores, PGO | 1,234.8 ms | 25 MB |
|  | without turns, all cores, PGO | 670.3 ms | 26 MB |

The profile changes little here: between 2% faster and 1% slower on one
core, within what runs differ by; on all cores from as fast to 6% faster
with squadrons of 1 and 10, 4 to 15% slower with 100, where a year takes
15 to 26 ms.
What it gains elsewhere (the epidemic and Doom some 17%, see
bench/RESULTS.md) it gains by inlining what is called often from more
than one place; here the time is in loops over the squadrons, in the sort
of `in turn` and in drawing numbers, which are inlined already.

Against the earlier entry, with squadrons of 100 the model takes 19.6 ms
where 24.4 (one core), without turns 10.7 where 11.3: since then systems
are fused on one core and functions are internal, which weigh most where
systems do little (which of the two gave how much was not measured). With
squadrons of 1 it is as it was: 1,092 ms with turns, 636 without, where
beecs takes 1,524.

## 2026-10-15, wyrand

`python3 bench/beecs/run.py --seeds 9 --claims`: as before, one core,
with the numbers at random of wyrand (`import wyhash`, one multiply to
128 bits, `mul_wide`) where SplitMix64's mix was (two multiplies, three
shifts), but in the loops that run on eight squadrons at a time.

| squadron size | | a year | memory |
|---|---|---|---|
| 100 | beecs (Go), one core | 23.0 ms | 17 MB |
|  | model, one core | 19.5 ms | 19 MB |
|  | without turns, one core | 10.1 ms | 21 MB |
| 10 | beecs (Go), one core | 158.8 ms | 17 MB |
|  | model, one core | 113.5 ms | 19 MB |
|  | without turns, one core | 65.2 ms | 21 MB |
| 1 | beecs (Go), one core | 1,510.2 ms | 17 MB |
|  | model, one core | 1,061.4 ms | 19 MB |
|  | without turns, one core | 603.7 ms | 21 MB |

With squadrons of 1, 1,061 ms where 1,092 with turns, 604 where 636
without (beecs 1,510 where 1,524). Each a whole year of one seed, the
median of 7 on CPU 2, seed 2:

| numbers at random | without turns | with turns |
|---|---|---|
| SplitMix64 | 630 ms | 1,086 ms |
| SplitMix64, each mixed twice | 755 | 1,201 |
| wyrand everywhere | 647 | |
| wyrand, SplitMix64 in `Draws` (`decide`) | 598 | 1,077 |
| ... and in the two shuffles of the turns | 601 | 1,053 |
| ... and lowbias32 (32 bits) in `Draws` | 602 | 1,078 |

- Mixing a second time costs some 125 ms: drawing is a fifth of the year
  without turns.
- wyrand everywhere is slower: the loop that draws for every squadron
  (`Draws`), which ran on eight at once with SplitMix64's multiplies to
  the low 64 bits (`vpmullq`), runs on one at a time, there being no
  vector multiply to the high 64 bits; `decide` took some 240 ms where
  215. Elsewhere wyrand gains.
- The shuffles of the turns (`shuffle_recruits`, `rest_in_line`) are
  such loops too. Without turns they do not run, and the 3 ms there come
  from where the code lies.
- A mix of 32 bits (lowbias32) in `Draws`, on sixteen at once, gains
  nothing more: that loop is not held up by its multiplies.

The model holds against beecs as before (check/curves.py, 400 seeds each,
both ways, all six scenarios); two tests that were off at 1% there
(`small` without turns, in-hive workers; `scarce` with turns, honey) agree
over seeds 1001 to 1800 and over 800 seeds.

## 2026-10-18, many colonies at once, on the GPU

`examples/beecs/many.ent`: 100 colonies side by side (seeds 1 to 100),
every forager a squadron of its own (`Foragers.SquadronSize 1`, 25,000
places a colony, 2.5 million in all), a year of the defaults, without
turns. On CPU 4 (the others of 4 to 7 for four threads), the GPU an RTX
4050 Laptop; the model built `-march=native`.

| | a year of 100 colonies |
|---|---|
| `many.ent`, one core | 186.4 s |
| `many.ent`, four threads (`--parallel`) | 184.2 s |
| `many.ent`, `--gpu` | 11.5 s |
| 100 runs of `beecs.ent` without turns, one after another | ~90 s (0.9 s each) |
| the same, four at a time on four cores | 23.1 s |

The three builds of `many.ent` say the same lines to the byte. With
the GPU, at first 17.0 s; then 12.6 s with the lists of who asked a
patch that is short made only in rounds where one is (in the defaults
none is) and `fly` split from the running sums that list the dancers and
the resting (whose first pass had worked out the whole flight again);
11.5 s with scans in chunks of 16 on the GPU. Merging a round's first
three passes into one, and the lists' pass reading two fields, gained
nothing more: a round is five passes over all places, about 250 bytes
each, some 2.7 ms at the card's bandwidth. `ENT_GPU_TIMES`, a round:
`fly` 0.52 ms, the colonies going through their dancers where fewer rest
than would follow (`settle`) 0.41, `decide` 0.38, the lists' running
sums 1.0 (two scans of 2.5 million rows), `come_home` 0.17.

## 2026-10-18, later: sums that count, as bits

The same 100 colonies. The lists' running sums (`+= 1` under an `if`)
are a bit for each place on the GPU, a word of 32 places scanned where a
place was (docs/gpu.md): the lists' passes 0.25 ms a round where they
took 1.0. A year 8.9 s on the GPU (11.5), the lines the same to the
byte as on the CPU. A round now: `fly` 0.53 ms, the colonies going
through their dancers 0.40, `decide` 0.39 (with its list of who passes
the filter), `come_home` 0.22, the lists 0.25.

## 2026-10-18, later still: `fly`

What every flier worked out alike for its patch is worked out once for
the patch a round (kilometres, the chance to give it up, a table of the
dances' Poisson chances), and loops are compacted only where their
guards read the row's own columns (docs/gpu.md). The same lines to the
byte. On CPU 8 (CPUs 0 to 7 busy with another session), alternately:

| | a year of 100 colonies, the GPU |
|---|---|
| before (d0cc498) | 8.17 s |
| now | 7.70 s |
| (not compacted at all) | (7.25 s) |

`ENT_GPU_TIMES`, a round: `fly` 400 us (525), `decide` 303 (327).
