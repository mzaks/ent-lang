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

## 2026-10-17: the modes, tried three ways

Run in one mode only, the dev set says which problems need which: grs is
solved focused (57 s, against 150 s switching) and asconhash stable,
but focused alone solves 27, stable alone 32, and switching 37. Three
changes to the modes were tried, each against entsat on the dev set:

| build (dev set) | solved | PAR-2 | geometric mean against entsat |
|---|---|---|---|
| entsat | 37 | 3,770 | 1 |
| modes as long in propagation work, not conflicts | 33 | 5,517 | |
| a saved phase for each mode | 34 | 4,956 | 0.82 |
| target phases from the stable mode only | 34 | 4,860 | 0.72 |
| both of these | 35 | 4,776 | 1.07 |

(The geometric mean is over the problems both solve.) The last two are
faster where they solve, but each loses problems that entsat solves,
and at 600 s these stay lost or come in late: ContextModel_6_4_6 takes
188 s, and 524 s with stable-only targets, though 6.8 s in stable mode
alone. One problem's time swings so with any change to the search. On
the other 61 problems, stable-only targets solves one more
(mchess18 35%, 208 s), 35 in all; entsat solves two of them (mchess18
35% in 184 s, 6g_6color_366 in 223 s), 39 in all. None of the three is
kept. MiniSat, pinned on all 100, solves the same 29 as on the dev set:
PAR-2 44,462.

## 2026-10-17: elimination as MiniSat orders it

shift1add took entsat 215 s and MiniSat 8 s, all of it in what was
left after preprocessing: MiniSat's elimination leaves 3 clauses of
the 1.1 million, entsat's left 543k. Three things made the difference,
each found by changing MiniSat until it left what entsat did:
subsumption run backward from every clause added or strengthened
(MiniSat with it off leaves 682k), variables taken cheapest first from
a heap and put back when their counts change (with a fixed order, 435k),
and ties taken by the lower variable. satprep now does all three, with
live counts kept per literal, a signature per clause, and a round over
the clauses on the variables of resolvents when the heap runs out. It
leaves 1 clause on shift1add, and as few or fewer than MiniSat on grs,
shuffling-2 and ContextModel. It is right on SATLIB, and drat-trim
checks its proofs of 10 structured problems, shift1add's among them.

| solver (all 100) | solved | SAT | UNSAT | PAR-2 | median | geometric mean |
|---|---|---|---|---|---|---|
| MiniSat 2.2 | 29 | 12 | 17 | 44,462 | 1 | 1 |
| entsat | 39 | 19 | 20 | 39,578 | 1.18 | 0.96 |
| entsat, elimination as MiniSat's | 37 | 19 | 18 | 40,131 | 1.14 | 0.64 |

shift1add takes 1.3 s, shuffling-2 31 s (91 s), mod2c 85 s (130 s), grs
95 s (121 s), Timetable_481 7.5 s (42 s), PRP 15% less. Three problems
are new: REGRandom (188 s), DivS (140 s) and em_11_3_4 (238 s, solved by
neither MiniSat nor entsat before). Five are lost, all of them long
searches whose times swing with any change (see above): asconhash H4 and
H6, ContextModel 6_3_8 and 6_4_6, mchess18 45%. mchess18 35% takes 285 s
(184 s).

## 2026-10-17: reason bumping, rephasing, vivification

Three things Kissat and CaDiCaL do, aimed at the long searches lost
above, each behind an option and tried on the dev set first:

| build (dev set) | solved | PAR-2 | median against MiniSat |
|---|---|---|---|
| entsat | 34 | 4,600 | 1.14 |
| reasons bumped, clauses of 10 literals or less | 36 | 3,796 | 0.96 |
| the same, 30 literals or less | 34 | 4,762 | 0.69 |
| 10, and rephasing (best, false, best, true) | 35 | 4,587 | 1.07 |
| 10, and vivification (LBD 6 or less) | 32 | 6,228 | 1.07 |

Reason bumping (`Options.reasons`): where the learned clause is short,
the variables of its literals' reasons are bumped too. Rephasing
(`Options.rephase`) sets every phase again at growing intervals, as
Kissat does without its walk. Vivification (`Options.vivify`) runs after
each reduction at level 0: each learned clause of LBD 6 or less has its
literals made false one by one and propagated without it, and is made
shorter where that falsifies a clause or makes another of its literals
true. It is right on SATLIB and drat-trim checks its proofs of 7 UNSAT
problems, and it is not slow in itself: on Timetable_481 it runs at
28,700 conflicts a second (5,900 without), but needs 1.55 million
conflicts where entsat needs 27,723, 54 s against 4.7 s. Not the return
to level 0 nor the probing: with the clauses probed but left as they
were it takes 8.1 s. The shorter clauses had the LBD k - 1 where that
was less, and so more of them were kept by reduce, for good at 2 or
less; with the old LBD kept Timetable_481 takes 10 s and mchess16 68 s
(112 s, entsat 62 s). On the dev set that solves 35, PAR-2 4,612,
against entsat with reasons bumped 36 and 3,813: the Timetables and
PRP_20_40 faster, asconhash H4, mod2c (81 s to 299 s) and two PRPs
slower. Both stay in, off.

| solver (all 100) | solved | SAT | UNSAT | PAR-2 | median | geometric mean |
|---|---|---|---|---|---|---|
| MiniSat 2.2 | 29 | 12 | 17 | 44,462 | 1 | 1 |
| entsat | 37 | 19 | 18 | 40,131 | 1.14 | 0.64 |
| entsat, reasons bumped (10) | 37 | 18 | 19 | 40,042 | 0.86 | 0.59 |

Bumping reasons wins back asconhash H4 (184 s) and H6 (166 s), and
mchess18 45% (272 s); H7 takes 137 s (196 s), grs 36 s (95 s), qwh.40
6.5 s (16 s). It loses REGRandom (188 s before), 6g_6color_366 (209 s)
and mchess18 35% (285 s), and the Timetables go from 7-32 s to 24-59 s,
shuffling-2 from 31 s to 50 s. The same count, then, with the gains on
the dev set it was chosen by and two of the losses off it: a small win,
kept as the default.

## 2026-10-18: memory, a budget, and reducing as CaDiCaL 3

REGRandom-K4-L1-Seed5 (1024 clauses of 514 literals) ran the machine
out of memory with four problems at a time: preprocessing strengthened
a clause by copying it, one literal at a time, and so took 3.45 GB.
Strengthened in place (MiniSat's strengthenClause) it takes 214 MB.
The subsumption after each resolvent then looked at 44 billion literals
there, 62 s before the search; it stops at a billion now (no other
problem of the dev set comes past 112 million), 1.6 s.

The search still did not solve it in 300 s; MiniSat does in 45 s, with
1.28 million conflicts. Nine variants of the search (one mode only,
Luby restarts, no target phases, no reason bumping, no trail reuse, no
preprocessing) did not either. Keeping more learned clauses did: a
reduction every 300 times the square root of the reductions so far
deletes about ten times as often as CaDiCaL 3.0.1, which waits 25 times
the square root of the conflicts, times log10(clauses / 10,000) above
100,000 clauses. CaDiCaL itself does not solve it in 300 s. On
REGRandom:

| reduce schedule | time | conflicts |
|---|---|---|
| 300 √reductions (before) | - | 26 million in 300 s |
| 300 √reductions, every 10,000 | 113 s | 9.2 million |
| 300 √reductions, every 30,000 | 40 s | 2.9 million |
| 25 √conflicts (CaDiCaL 3) | 130 s | 10.5 million |
| 100 √conflicts | 51 s | 3.1 million |

On the dev set, each run three times (the problems are the same in
each, the times near the limit not):

| solver (dev, 39) | solved | PAR-2 | median against MiniSat |
|---|---|---|---|
| entsat, reasons bumped | 36 | 3,813 | 0.86 |
| 25 √conflicts, the first after 25 | 37, 36, 37 | 3,054, 3,580, 3,197 | 0.41 |
| 25 √conflicts, the first after 300 (as CaDiCaL) | 35, 35, 35 | 4,117, 4,170, 4,153 | 0.77 |
| 50 √conflicts | 36 | 3,746 | 0.55 |
| 100 √conflicts | 34 | 4,517 | 0.70 |
| 300 √reductions, every 30,000 | 34 | 4,615 | 0.73 |

25 √conflicts, the first after 25, is the default: PRP, the Timetables,
shuffling-2 and bhaskara faster, asconhash H6 and H7 slower. SATLIB all
correct; drat-trim checks the proofs of 11 dev problems, REGRandom's
too. (A proof of a gigabyte or more fills /tmp, a tmpfs, where four are
checked at a time; it is then cut short and does not verify.)

Vivification as Kissat does it (before a reduction, on the clauses used
since the last one, literals in the order of how many tried clauses
have them, clauses sorted so that one keeps the levels of the one
before) solves 35, PAR-2 4,193: better than before (32, 6,228), still
off.

## 2026-10-19: subsumption along the lists without going to the clauses

DivS_942_11 (936,584 clauses of 11 literals on 942 variables) took 109 s
to preprocess, more than MiniSat takes to solve it (98 s); preprocessing
alone solves it. The lists of a literal are some 5,000 clauses long, and
each clause on one cost two reads of `Pre` far apart, its flags and its
signature, to be passed. Each clause on a list now has its signature
beside it, as it was when it was put there: strengthened since, it has
fewer bits, so one that has not all of the other's bits is passed
without going to `Pre`. With the list's start and length kept while it
is walked, DivS takes 8.8 s, all 100 problems 32 s instead of 132 s
(battery numbers before were 200 s and 2x). Deleted clauses leave the
lists later, so the clauses are visited in another order and 18 problems
keep up to 0.05% other clauses.

| solver (dev, 39, on mains) | solved | PAR-2 | median against MiniSat |
|---|---|---|---|
| entsat (reduced as CaDiCaL 3) | 37 | 3,037 | 0.41 |
| signatures on the lists | 38 | 2,425 | 0.55 |

DivS 132 s to 9 s, REGRandom 129 s to 54 s, ContextModel 6_3_8 solved
(24 s); PRP_30_41, PRP_30_39, PRP_20_40 and mchess16 slower (twice). SATLIB
all correct, 11 dev proofs verified.

## 2026-10-19: local search before the search

`satwalk.ent` is probSAT (Balint and Schoening) as Kissat's walker
weights it: a false clause at random, one of its variables flipped with
the weight cb to the power of minus its break, cb by the clauses' mean
length. It walks the clauses the search starts with, from the phases;
a model it finds is the answer, else its best values become the phases.
The best values are kept as Kissat keeps them, by the variables flipped
since: copied whole each time, a walk of 10 million flips managed 65,536
on PRP in a second. It finds models of uf250 in 1 to 300 ms (7 to 11
million flips a second; 1 to 2 million on a million clauses).

It was tried for sgen3-n260 (260 variables, 624 clauses, SAT), which
entsat, MiniSat and CaDiCaL do not solve in 300 s: in 300 s, 3.26
billion flips, the fewest false 3; with cb from 2.0 to 6.5 for 60 s
each, 2 or 3. On the dev set, alone it solves 3col120 and satsgi (which
the search solves at once). Before the search, 10 million flips:

| solver (dev, 39) | solved | PAR-2 | median against MiniSat |
|---|---|---|---|
| entsat | 38 | 2,425 | 0.55 |
| walk first, best values copied whole (65,536 flips on PRP) | 37 | 3,379 | 1.07 |
| walk first, from the phases, as Kissat keeps the best | 37 | 3,188 | 1.23 |

mod2c 68 s to 12 s; the PRPs, Timetable_484 (7 s to 27 s), REGRandom
and minxor slower, ContextModel 6_3_8 lost, and every small problem the
walk's second or more. So it stays off (`Walk.limit = 0`, where the
search is as before, conflict for conflict), and many walkers at once on
the GPU are not worth the language they need (a `for` reading the rows
of a buffer it writes) on these problems.
