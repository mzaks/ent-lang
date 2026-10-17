# entsat: timings

`examples/sat` against MiniSat 2.2 (github master, the `simp` binary with
its preprocessing, as MiniSat runs by default) on SAT Competition
problems, scored as competitions score: how many are solved within the
time limit, and PAR-2, the seconds of each solved problem plus twice the
limit for each that is not.

The problems are a fixed sample of the 2023 main track (`sample.txt`,
made by `sample.py`): the track's 400 problems from the Global Benchmark
Database, those of at most 20 MB compressed (364), in the order of their
hashes, 100 evenly spaced. Many of them are hard: at 300 s both solvers
leave about three quarters unsolved.

AMD Ryzen AI 9 HX 370 (4 Zen 5 cores, 8 Zen 5c, 24 threads), Arch
Linux, LLVM 22.1.8, entsat built `-O2 -march=native`, MiniSat as its
Makefile builds it (`-O3`). `bench/sat/run.py --limit 300 --jobs 8`:
eight problems at a time, the limit 300 s; entsat without the proof.

## 2026-10-16: phase 1 against MiniSat

| solver | solved | SAT | UNSAT | PAR-2 |
|---|---|---|---|---|
| entsat, phase 1 (VMTF, Luby restarts, LBD reduce, no preprocessing) | 26 | 16 | 10 | 46,259 |
| MiniSat 2.2 | 28 | 11 | 17 | 45,148 |

PAR-2 1.02 times MiniSat's: within the 3x the design doc's phase 2 gate
asks for. The two solve different problems. entsat solves five
timetabling problems (SC23_Timetable) MiniSat does not, and sgen1 in 5 s
where MiniSat takes 171; MiniSat solves the hash and XOR problems
(asconhash, minxor128, grs, shift1add) and php-010-009 in 2 s where
entsat takes 98. Those are where its preprocessing and VSIDS are strong.

## 2026-10-16: two modes, tiers, elimination

The same, with what phase 2 added: Glucose restarts, the stable mode
(VSIDS, Luby restarts by 1,024, target phases) between focused ones, the
LBD tiers (LBD 2 kept, up to 6 kept while used, three quarters of the
rest deleted every 300 times the square root of the reductions so far
conflicts, LBDs worked out again when a clause is used), the bumped
variables sorted by radix, and bounded variable elimination before the
search (`satprep.ent`).

| solver | solved | SAT | UNSAT | PAR-2 |
|---|---|---|---|---|
| entsat, phase 1 | 26 | 16 | 10 | 46,259 |
| MiniSat 2.2 | 28 | 11 | 17 | 45,148 |
| entsat | 33 | 16 | 17 | 43,063 |

PAR-2 0.95 times MiniSat's, and no answer that another solver gives
otherwise. What the elimination and VSIDS bring is where MiniSat was
ahead: the three asconhash problems, minxor128, grs-64-32, a shuffling
problem and ContextModel are solved now, and sat-bench-trig-bhaskara,
which MiniSat does not solve; PRP_30_41 is lost (119 s before).

On SATLIB's random 3-SAT, with 24 runs at once, uuf250 takes 5.4 s on
average where it took 38 s in phase 1 (MiniSat: 4.1 s).

## 2026-10-16: subsumption and strengthening

The same, with backward subsumption and strengthening (self-subsuming
resolution) before the elimination: each clause of at most 20 literals
looks along the list of its least frequent literal, both ways.

| solver | solved | SAT | UNSAT | PAR-2 |
|---|---|---|---|---|
| MiniSat 2.2 | 28 | 11 | 17 | 45,148 |
| entsat, without subsumption | 33 | 16 | 17 | 43,063 |
| entsat | 34 | 17 | 17 | 41,912 |

PAR-2 0.93 times MiniSat's. The problems that changed are near the limit
both ways: PRP_30_41 and ContextModel_6_4_6 are solved now (229 s, 118 s),
ContextModel_6_3_8 is not (260 s before).

## 2026-10-16: binary clauses, the trail kept, unused variables

Three changes. A clause of two literals is watched with its other
literal as the blocker, and propagated without looking at the clause. A
restart keeps the levels whose decisions come before the next one (as
CaDiCaL reuses the trail). A variable in no clause, as read or after the
elimination, is false at level 0 and no longer decided: in asconhash H6,
80,000 of 127,069 variables, which the search decided over and over
(43 decisions a conflict where MiniSat makes 4).

Two of them were run side by side, four problems at a time each.
"Median" is the median of entsat's time over MiniSat's on the problems
both solve (24 for each build).

| solver | solved | SAT | UNSAT | PAR-2 | median |
|---|---|---|---|---|---|
| MiniSat 2.2 | 28 | 11 | 17 | 45,148 | 1 |
| entsat, before | 34 | 17 | 17 | 41,912 | 1.55 |
| unused variables, trail kept | 34 | 16 | 18 | 42,420 | 1.44 |
| and binary clauses, trail not kept | 34 | 16 | 18 | 42,546 | 1.29 |
| entsat (all three) | 35 | 18 | 17 | 41,875 | 1.15 |

PAR-2 0.93 times MiniSat's. More conflicts a second are not a faster
search: asconhash H6 has nine times as many in 300 s and is not solved
(216 s before). Builds that search differently take very different
times on one problem (qwh: 7.5 s and 26.8 s); a build itself searches the
same each time, but eight runs at once land on cores of 5.2 and of 3.3
GHz, so a few percent are not told apart here (see below). MiniSat is still ahead on most of the long problems both
solve: grs, PRP, asconhash, shuffling-2.

## 2026-10-17: on like cores, and row numbers

`run.py` now runs each problem on a core of its own, the four fast ones
by default (`--cores 0-3`), four at a time. entsat searches the same
each run; timed twice so, its times on the 30 problems both runs solve
are within 4% of each other (the median 0.996), and every answer the
same. For trying builds, `--set dev` is the 39 problems that any solver
has solved (`dev.txt`); the other 61 cost 300 s each and tell nothing.
The ratio to MiniSat is taken on the problems both solve where one takes
a second or more.

Row numbers (`index of Arena` and the like: rows read and written
without a check, masked to a buffer's room) make entsat about 12%
faster with the same search, every problem of a second or more by 1 to
21%.

| solver (dev set) | solved | SAT | UNSAT | PAR-2 | median | geometric mean |
|---|---|---|---|---|---|---|
| MiniSat 2.2 | 29 | 12 | 17 | 7,862 | 1 | 1 |
| entsat, checked rows | 37 | 18 | 19 | 4,090 | 1.33 | 1.09 |
| entsat, row numbers | 37 | 18 | 19 | 3,770 | 1.18 | 0.96 |

On the 20 problems both solve, entsat takes 1,773 s, MiniSat 1,716 s.
On SATLIB's uuf250, 24 at once: 3.7 s on average (MiniSat 4.1 s).
