# The city's epidemic: timings

`examples/epidemic` against its reference, OpenABM-Covid19 (commit
367a0d3, built from its C with `cc -O3 -march=native` and GSL 2.8).

## 2026-10-17, later: `chance` on the GPU too

Sends from loops of a `gpu system`'s body (docs/gpu.md). The same
machine, cores and caveat as the entry below: `BENCH_ONE_CPU=4
BENCH_CPUS=4-7 BENCH_ALL=0 python3 bench/epidemic/run.py 1000000 --gpu
workplace,infectiousness,chance`.

| a million people, a day in ms | one core | 4 threads | memory MB |
|---|---|---|---|
| the CPU alone | 179.2 | 67.5 | 919 |
| `workplace`, `infectiousness` and `chance` on the GPU | 22.4 | 15.7 | 851 |

857,886 infected in every run. On the GPU `workplace` takes 1.75 ms and
`chance` 5.2 (4.98 at 4 threads), where on the CPU they took 87.7 and
73.8 on one core, 37.3 and 21.5 on four threads: the hazard stays on the
GPU between them, and comes back to the CPU once a day, for `infect`.

## 2026-10-17: the hazard in whole numbers, and `workplace` on the GPU

`Hazard.left` and `Household.pressure` count 2^-32ths (docs/gpu.md, "Audit:
the epidemic"). Measured on the slow cores, while another experiment had
the four fast ones: `BENCH_ONE_CPU=4 BENCH_CPUS=4-7 BENCH_ALL=0 python3
bench/epidemic/run.py 1000000 [--gpu workplace,infectiousness]`; an RTX
4050 Laptop GPU. Not for comparing with the entries below, which ran on
the fast cores.

| a million people, a day in ms | one core | 4 threads | memory MB |
|---|---|---|---|
| the hazard an f32 | 172.9 | 61.2 | 859 |
| in whole numbers | 178.7 | 68.1 | 919 |
| in whole numbers, `workplace` and `infectiousness` on the GPU | 92.6 | 35.7 | 856 |

857,886 infected in a hundred days in every run. `workplace` takes 4.8 ms
on the GPU where it took 89.2 on one core and 37.9 on four threads (a
`gpu` system's time is what it takes to start it and what the CPU waits
for it after; the GPU's copies of the hazard back and forth are in it).
In whole numbers `chance` sends 8 bytes where it sent 4: 21.4 ms on four
threads where it took 16.2.

## 2026-10-13, last: with what is done about it (staying at home, tests, tracing)

The same machine and builds as below, with `interventions.ent` in every
run: `python3 bench/epidemic/run.py 100000 1000000 10000000` for the
baseline, where none of the measures is on, and `... --tracing` with all
of them (who has symptoms stays at home with their household and is
tested; whom their app saw them meet in the last week is told, stays at
home and is tested; 500 infected at the start). The row for all the
cores is now the median of three runs, each time by itself: nothing
holds a thread to a core there, so what one thread does lands on a fast
core or a slow one, and single runs were apart by a third. One core
(CPU 2) and 4 threads (the four fast cores) are one run each.

| people | | set-up | a day | memory | infected by day 100 |
|---|---|---|---|---|---|
| 100,000, baseline | model, one core | 0.14 s | 7.5 ms | 135 MB | 85,658 |
| | model, 4 threads | 0.14 s | 4.1 ms | 140 MB | 85,658 |
| | model, all | 0.17 s | 4.3 ms | 138 MB | 85,658 |
| 1,000,000, baseline | model, one core | 1.76 s | 106.7 ms | 859 MB | 857,886 |
| | model, 4 threads | 1.74 s | 46.5 ms | 915 MB | 857,886 |
| | model, all | 1.76 s | 31.9 ms | 913 MB | 857,886 |
| 10,000,000, baseline | model, one core | 21.2 s | 1,890 ms | 8,755 MB | 8,519,012 |
| | model, 4 threads | 20.0 s | 692 ms | 9,239 MB | 8,519,012 |
| | model, all | 20.0 s | 445 ms | 9,238 MB | 8,519,012 |
| 100,000, tracing | reference, one core | 0.23 s | 35.8 ms | 382 MB | 31,587 |
| | model, one core | 0.14 s | 9.4 ms | 211 MB | 32,718 |
| | model, 4 threads | 0.14 s | 5.1 ms | 222 MB | 32,718 |
| | model, all | 0.17 s | 5.6 ms | 221 MB | 32,718 |
| 1,000,000, tracing | reference, one core | 2.50 s | 470 ms | 3,700 MB | 256,958 |
| | model, one core | 1.77 s | 107.5 ms | 1,376 MB | 266,034 |
| | model, 4 threads | 1.68 s | 54.9 ms | 1,412 MB | 266,034 |
| | model, all | 1.68 s | 43.6 ms | 1,411 MB | 266,034 |
| 10,000,000, tracing | model, one core | 21.2 s | 991 ms | 12,948 MB | 1,430,174 |
| | model, 4 threads | 20.0 s | 499 ms | 13,136 MB | 1,430,174 |
| | model, all | 20.3 s | 405 ms | 13,165 MB | 1,430,174 |

(The reference's two rows are from a run of the script before the last
changes to the model, which it is not part of; a check program of mine
was then busy on another core.)

What a day is made of, in milliseconds (`remember`, `symptoms`, `tests`,
`trace` and `told` are the measures' systems):

| people | | remember | infectiousness | pair | household | workplace | chance | infect | progress | symptoms | tests | trace | told | tally |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1,000,000, baseline | one core | 0.26 | 2.07 | 1.54 | 2.26 | 63.7 | 26.9 | 1.72 | 1.27 | 6.10 | 0.41 | 0 | 0.20 | 0.23 |
| | 4 threads | 0.14 | 0.58 | 1.78 | 1.48 | 28.1 | 11.0 | 0.81 | 0.56 | 1.71 | 0.20 | 0 | 0.08 | 0.09 |
| | all | 0.12 | 0.32 | 2.81 | 1.21 | 19.2 | 6.1 | 0.49 | 0.51 | 0.98 | 0.20 | 0 | 0.06 | 0.14 |
| 1,000,000, tracing | one core | 0.27 | 1.02 | 2.31 | 1.84 | 16.9 | 8.7 | 1.37 | 1.18 | 6.16 | 2.16 | 48.9 | 16.4 | 0.24 |
| | 4 threads | 0.18 | 0.64 | 2.67 | 1.43 | 9.3 | 3.6 | 0.69 | 0.51 | 1.67 | 0.77 | 24.6 | 8.7 | 0.20 |
| | all | 0.16 | 0.26 | 2.98 | 1.28 | 7.4 | 2.4 | 0.42 | 0.45 | 0.90 | 0.38 | 19.4 | 7.0 | 0.52 |
| 10,000,000, baseline | one core | 3.1 | 19.7 | 18.1 | 21.2 | 771 | 961 | 15.7 | 12.0 | 60.1 | 4.2 | 0 | 2.0 | 2.2 |
| | all | 1.9 | 3.7 | 19.8 | 10.2 | 209 | 182 | 2.9 | 4.7 | 8.3 | 1.4 | 0 | 0.3 | 0.9 |
| 10,000,000, tracing | one core | 2.9 | 8.2 | 23.6 | 15.4 | 106 | 86.9 | 7.5 | 8.8 | 60.4 | 17.1 | 473 | 179 | 2.3 |
| | all | 2.0 | 3.2 | 24.0 | 9.9 | 42.4 | 15.6 | 1.5 | 3.7 | 8.1 | 3.2 | 210 | 79.9 | 1.4 |

- With every measure on, a day of a million people takes the model 44
  ms on all the cores and 108 on one, where the reference takes 470 on
  one: an eleventh and a quarter. It has 1.4 GB where the reference has
  3.7. Both end with a quarter of the people infected where the baseline
  has six in seven (the two counts differ as two seeds of either do).
- Telling those one met is half of such a day (`trace`), and looking
  along one's edges whether one is still told a sixth (`told`). `trace`
  gains least from the cores (2.5 times on 24 threads): what it sends
  and the edges it connects are put on lists by all the threads and
  taken off by one. Fewer are ill, so the rest of the day costs a third
  of the baseline's.
- The baseline pays for the measures being there: on one core 107 ms
  for a day of a million where it took 90, since everybody has their
  state for them, and the pairs of each household are edges (913 MB
  where it had 725), and `workplace` reads of
  both ends of a tie whether they are kept at home. On all the cores it
  takes what it took (31.9 ms; 31.6 before). Where nobody is told about
  anybody, `trace` is not gone through at all.
- Ten million people with every measure on: 405 ms a day on all the
  cores, in 13 GB. The eight days of where everybody's stubs were are 640
  MB of that; the edges of who is told about whom, and what is sent
  along them, most of the rest.

## 2026-10-13, later: sums each thread for itself, and the meetings by chance on all the cores

The same machine, runs and builds as below, after two changes to the
compiler that the timings below asked for (their last two points): whole
numbers added into a unique are a parallel loop's reductions, and a
`for` may send to another entity from a loop of its body, so the
meetings by chance are a system like the others and no proc
(`python3 bench/epidemic/run.py 100000 1000000 10000000`).

| people | | set-up | a day | before | memory | before | infected by day 100 |
|---|---|---|---|---|---|---|---|
| 100,000 | model, one core | 0.14 s | 6.3 ms | 6.3 | 103 MB | 97 | 85,813 |
| | model, 4 threads | 0.17 s | 4.7 ms | 5.1 | 108 MB | 124 | 85,813 |
| | model, all | 0.21 s | 4.0 ms | 5.0 | 104 MB | 125 | 85,813 |
| 1,000,000 | model, one core | 1.66 s | 90.4 ms | 89.8 | 671 MB | 665 | 857,656 |
| | model, 4 threads | 1.58 s | 43.9 ms | 59.9 | 727 MB | 850 | 857,656 |
| | model, all | 1.59 s | 31.6 ms | 53.1 | 725 MB | 851 | 857,656 |
| 10,000,000 | model, one core | 23.2 s | 1,653 ms | 1,609 | 6,755 MB | 6,680 | 8,520,877 |
| | model, 4 threads | 19.9 s | 662 ms | 1,251 | 7,227 MB | 8,178 | 8,520,877 |
| | model, all | 20.4 s | 422 ms | 1,192 | 7,263 MB | 8,176 | 8,520,877 |

| people | | infectiousness | pair | household | workplace | chance | infect | progress | tally |
|---|---|---|---|---|---|---|---|---|---|
| 100,000 | one core | 0.20 | 0.14 | 0.22 | 3.44 | 2.01 | 0.19 | 0.11 | 0.02 |
| | all | 0.05 | 0.83 | 0.29 | 1.78 | 0.84 | 0.06 | 0.09 | 0.07 |
| 1,000,000 | one core | 1.97 | 1.33 | 2.36 | 56.9 | 24.7 | 1.74 | 1.22 | 0.23 |
| | 4 threads | 0.64 | 1.35 | 1.54 | 27.5 | 11.2 | 0.89 | 0.66 | 0.12 |
| | all | 0.37 | 2.70 | 1.38 | 18.6 | 7.1 | 0.57 | 0.69 | 0.14 |
| 10,000,000 | one core | 19.4 | 13.0 | 21.4 | 655 | 912 | 16.3 | 12.7 | 2.3 |
| | 4 threads | 6.1 | 14.7 | 12.1 | 295 | 323 | 4.9 | 5.2 | 1.4 |
| | all | 4.1 | 17.1 | 10.5 | 201 | 179 | 3.3 | 5.2 | 1.5 |

- Every run is the one it was: the same people infected, on one core
  and on any number of threads (`check/same.py`).
- On all the cores a day at a million takes 31.6 ms where it took 53.1,
  and at ten million 422 ms where it took 1,192: 2.9 and 3.9 times less
  than one core, where it was 1.7 and 1.35. Against the reference at a
  million (590 ms on one core) that is a nineteenth.
- The meetings by chance take 7.1 ms at a million on all cores (24.6
  before) and 179 ms at ten million (905). What is found for each stub
  is found on every thread; what is sent is then taken off the lists by
  one, a write somewhere in the city for each, which is what is left of
  it. One core is as it was: there each value lands where it is sent.
- `tally` takes on all cores what it takes on one or less (0.14 ms at a
  million where it took 6.1; 1.5 at ten million where it took 57.8), and
  the twelve places for each person that it filled are not touched: a
  seventh less memory at a million, 900 MB less at ten million.
- The largest part of a day on all cores is now the ties of work at
  every size (18.6 of 31.6 ms at a million).

## 2026-10-13: 100,000 to ten million people

AMD Ryzen AI 9 HX 370 (4 Zen 5 cores at 5.16 GHz and 8 Zen 5c at 3.29,
24 threads, 22 GB), Arch Linux, LLVM 22.1.8, the model built with
`-O2 -march=native`. `python3 bench/epidemic/run.py 100000 1000000
--reference` and `... 10000000`: one run each of 100 days of the
reference's baseline (ten infected at the start, seed 1), both from the
same parameter file. One core is CPU 2 (a Zen 5 core) for the model and
the reference alike; "4 threads" are the four Zen 5 cores, "all" every
thread of the machine. The machine was otherwise idle; nothing was
repeated, so differences of a few percent mean nothing.

A day is the time of the 100 days shared among them, the city's
building apart (the reference's: from its first day's line to its last,
so the files it writes at its end do not count). Memory is the most the
process had.

| people | | set-up | a day | memory | infected by day 100 |
|---|---|---|---|---|---|
| 100,000 | reference, one core | 0.20 s | 43.8 ms | 401 MB | 85,886 |
| | model, one core | 0.14 s | 6.3 ms | 97 MB | 85,813 |
| | model, 4 threads | 0.14 s | 5.1 ms | 124 MB | 85,813 |
| | model, all | 0.16 s | 5.0 ms | 125 MB | 85,813 |
| 1,000,000 | reference, one core | 2.28 s | 590 ms | 3,825 MB | 858,567 |
| | model, one core | 1.65 s | 89.8 ms | 665 MB | 857,656 |
| | model, 4 threads | 1.58 s | 59.9 ms | 850 MB | 857,656 |
| | model, all | 1.63 s | 53.1 ms | 851 MB | 857,656 |
| 10,000,000 | model, one core | 21.2 s | 1,609 ms | 6,680 MB | 8,520,877 |
| | model, 4 threads | 19.9 s | 1,251 ms | 8,178 MB | 8,520,877 |
| | model, all | 19.8 s | 1,192 ms | 8,176 MB | 8,520,877 |

The reference was not run with ten million: by its memory at a million
it would need some 38 GB, and the machine has 22. Kept for one day in
place of ten, the people everybody met (which the model does not keep
at all) leave the reference 140 MB at 100,000 and 1,313 MB at a million:
the model has 0.7 and 0.5 of that.

What a day is made of, in milliseconds (the systems of
`transmission.ent` and `disease.ent`; `tick` and `policy` take none):

| people | | infectiousness | pair | household | workplace | chance | infect | progress | tally |
|---|---|---|---|---|---|---|---|---|---|
| 100,000 | one core | 0.20 | 0.13 | 0.22 | 3.38 | 2.06 | 0.18 | 0.11 | 0.02 |
| | all | 0.07 | 0.15 | 0.27 | 1.69 | 2.09 | 0.14 | 0.10 | 0.51 |
| 1,000,000 | one core | 1.95 | 1.24 | 2.28 | 56.2 | 24.9 | 1.73 | 1.24 | 0.23 |
| | 4 threads | 0.94 | 1.23 | 1.34 | 24.5 | 24.6 | 0.82 | 0.61 | 5.88 |
| | all | 0.72 | 1.48 | 1.24 | 17.7 | 24.6 | 0.57 | 0.71 | 6.10 |
| 10,000,000 | one core | 19.7 | 12.4 | 21.4 | 628 | 896 | 16.0 | 12.2 | 2.3 |
| | 4 threads | 5.8 | 12.4 | 11.2 | 270 | 885 | 4.8 | 4.9 | 57.1 |
| | all | 3.8 | 12.7 | 9.8 | 194 | 905 | 3.0 | 4.9 | 57.8 |

The city's building at ten million, one core: the households taken 8.9
s, the people spawned 1.5 s, the networks' 70 million ties 10.6 s.

### What holds

- On one core the model takes a seventh of the reference's time for a
  day at 100,000 people and at a million (6.9 and 6.6 times less), and
  on all cores an eleventh at a million. The reference builds every
  meeting of every person every day and keeps ten days of them; the
  model keeps the ties of work as edges, asks a number for which of them
  are met, and works a day's meetings by chance out from the stubs of
  those who are ill.
- A run is the same on one core and on any number of threads, to every
  person's state on every day (`examples/epidemic/check/same.py
  1000000`), and so the infected are the same in every row of a size.
- From a million people to ten million a day costs 18 times as much,
  not ten: what a person gives off goes to people anywhere in the city,
  and the columns it is read from and written to (40 MB each with ten
  million) no longer fit the processor's cache (16 MB). The meetings by
  chance grow 36 times, the ties of work 11 times.
- All the cores take 1.7 times less than one at a million and 1.35 at
  ten million, since the largest part is on one core: the meetings by
  chance are a proc, 46% of a day on all cores at a million and 76% at
  ten million. A `for` cannot do it: what an entity sends to another
  (`Hazard(whom).left -= ..`) is one value for each place it is written
  at, and an ill person meets several, how many being a number of their
  own. Edges can carry that (the ties of work do, and run on all
  cores: 3.2 times faster on 24 threads), but then the day's meetings
  must be made as edges, which was the larger cost (below).
- Sums into a unique cost more on all cores than on one (`tally`: 6.1
  ms against 0.23 at a million): each entity's value goes to a place of
  its own and all are added up after, on one core, where one core just
  adds as it goes.

### How it got here (a million people, one core, a day)

| | a day | pair | workplace | chance |
|---|---|---|---|---|
| a stub for every meeting laid out, the pairs made edges of a relation that is emptied and filled each day | 187 ms | 80.7 | 88.7 | 10.3 |
| the meetings worked out for the ill alone (an order of the stubs that is worked out both ways); one number asked for a tie in place of five | 132 ms | 0.7 | 56.1 | 67.2 |
| whose a stub is, looked for near where it would be were everybody alike | 106 ms | 0.7 | 56.3 | 41.2 |
| ... by an index of every 64th stub | 90 ms | 1.2 | 56.2 | 24.9 |

On all cores the first row was no faster (186 ms): `workplace` did not
run in parallel, because its `for` calls a fn that is given a buffer
(the networks' numbers), which the compiler took for something a
parallel loop may not do. It may: a buffer is read like a unique. With
that, `workplace` takes 17.7 ms on all cores.

Before the first row, at 100,000 people, the order of the stubs was a
Feistel network over a power of four (27 ms a day) and then a stirring
of so many bits (13 ms).

### Who is ill, stored apart

`tag Ill apart` puts the ill in an archetype of their own, so that a
`for` over them goes over just these (`run.py 1000000 --apart`, with
the meetings worked out for the ill alone, before the index):

| a million people | a day | pair | household | workplace | chance | memory | infected |
|---|---|---|---|---|---|---|---|
| in place, one core | 105 ms | 0.7 | 2.3 | 56.1 | 40.4 | 675 MB | 857,656 |
| apart, one core | 229 ms | 3.6 | 7.3 | 119.9 | 92.5 | 809 MB | 856,799 |
| in place, all | 71 ms | 1.0 | 1.1 | 17.6 | 43.0 | 860 MB | 857,656 |
| apart, all | 156 ms | 4.3 | 7.2 | 40.8 | 92.7 | 1,062 MB | 856,799 |

Apart takes twice as long. Nothing is gained, since in place a `for`
over the ill already costs next to nothing where few are ill (with one
person ill in a million, `workplace` takes 0.7 ms and `chance` 0.7);
and much is lost, since a person who moves to another archetype is no
longer found by their number as a row: every tie, every meeting and
every member of a household is then looked up in a table. (The run is
another one, too: what several send to one person is added up in the
order of the archetypes and rows, so the sums differ in their last bits,
and with them who is infected on which day.)
