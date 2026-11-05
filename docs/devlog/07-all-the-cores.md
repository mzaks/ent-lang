# 07 · All the cores, deterministically

*Mostly 13 to 22 October 2026, on an AMD Ryzen AI 9 HX 370: 4 fast Zen 5
cores, 8 slower Zen 5c cores, 24 threads.*

## The hard rule

A simulation that gives other answers on another machine can't be debugged.
You can't replay it or compare it, and you can't trust it. So ent-lang has
one rule that everything in this post bends around:

> **The same program gives the same bytes on one thread, on 24, on the GPU,
> and on many processes.** Where its entities live and how many threads run
> changes how fast it is, never what it computes.

This is checked, not hoped for. `examples/epidemic/check/same.py` builds the
epidemic for one core and for all, and runs it on 1, 4, 16 and 24 threads.
Each day it writes a number made of everything about every person, and the
lines must be identical.

## Where nondeterminism comes from, and how each source was closed

**1. Random numbers.** A random generator that is a stream is a race. Whoever
draws first changes everyone else's numbers. The epidemic's `random.ent`
keeps no generator. A draw is computed from what it is *for*:

```
fn key(seed: i64, who: i32, day: i32, stream: i32) -> u64 {
  let a = mix(seed as u64)
  let b = mix(a ^ (who as i64 as u64))
  let c = mix(b ^ (day as i64 as u64))
  mix(c ^ (stream as i64 as u64))
}
```

Person 4711's draw on day 12 for "progress" is the same number whichever
thread asks, and in whichever order. Later models did the same. (A wide
multiply, `mul_wide`, and a wyrand library module came out of tuning this
for beecs.)

**2. Who sees what.** That was settled on day one and written down in
`docs/sync-points.md`. *A query is the unit of consistency.* It sees the
world as it was when it started, plus its own entity's changes at once.
What it does to other entities lands when it ends. Bodies don't see each
other, so the compiler may split them across threads freely.

**3. Sums.** Float addition doesn't associate, so a parallel sum is a lottery
unless the order is fixed. In ent-lang, what a `for` sends to other
entities (`Hazard(who).left -= i.rate`, `Pull(o).fx += f`) is collected on
lists and combined when the `for` ends, in the order of the entities that
sent it:

> the same sums to the last bit, on any number of threads.

Whole-number sums into a unique are order-free, so since 13 October they
are plain reductions, one partial per thread. Floats stay ordered.

**4. Order the program cares about.** Sometimes "who goes first" is the
semantics. In Doom, every thing acts in turn and sees what the one before
did. So on 10 October `for` got an order:

```
for e, m: mut Mobj in turn by m.id {
  let roll = Random.index            // as the one before left it
  Random.index = (roll + 1) & 0xff
  ...
}
```

`by field` visits entities in key order, and remains one `for` with one end.
`in turn by` makes each entity a `for` of its own, one after another. That
is sequential by definition, so it's correct, and it's one core. The first
version placed entities by insertion. In beecs, with random keys, that was
quadratic. A radix sort over only the bytes in which the keys differ fixed
it, and the sort later ran on many threads too.

## Making all the cores pay

Correct was the easy half. The epidemic on 13 October, a day for a million
people:

- one core: 89.8 ms;
- all 24 threads: **53.1 ms**. That's a 1.7× speed-up from 24 threads.
  Embarrassing.

The model itself said why. Sums went through a single cell, and the
meetings by chance were a proc on one core, because a `for` couldn't send
from a loop inside its body. Both became language features the same day.
The result was 31.6 ms at a million (2.9× one core), and 422 ms against
1,192 at ten million (3.9×). It was the same people infected either way.

Then the **serial work between parallel loops** became the problem. In the
epidemic with tracing on 24 threads, the main thread was combining sends
while 23 threads waited:

- grouping the sends by target id and combining the groups in parallel;
- a counting sort of the `ToldBy` edges that places them back to front, and
  then a parallel one;
- reading the send lists a block at a time.

A run went from about 9 s to 4.85 s, to the same bytes. Which way to combine
(one list or groups) is now chosen at run time per send site and size,
with hysteresis. Without hysteresis, single spikes flipped the choice and
the snn benchmark got 12% slower.

## Things we read, and what they were worth

On 20 October I asked Claude to read the HyPer **morsel** paper, which hands
work to threads in small pieces and lets idle threads steal. The obvious
experiment was dynamic OpenMP scheduling for our loops. It lost: 63.6 ms per
day against static's 41.4 on the four fast cores. It hurt most where a small
archetype has heavy rows. Reverted. Reading a paper is cheap, but believing
it costs a measurement.

Then **Michael Jungmair's thesis** on LingoDB. It gave us two things:

- **`ent build --trace`** times every system, sort and parallel region per
  thread, and writes Chrome JSON. Its first finding: the epidemic's loops
  were only 50–70% busy, because threads slept between loops and woke late.
  The OpenMP wait before sleeping went from 1 ms to 5 ms, and the run went
  from 4.68 s to 4.37 s. A spinning sibling thread on CPU 0's core made
  serial work 2× slower. So the default became one thread per hardware
  thread, bound, except CPU 0's sibling. (For programs with a window the
  wait is 0, after Pixalorien's title screen kept 12 threads spinning.)
- **`ent build --tune trace.json`** feeds a trace back into the compiler:
  which loops are worth threads, which schedule, and from how many values to
  combine sends in groups. It gave modest gains: the epidemic 11.0 → 10.7 ms
  per day, LJ 0.198 → 0.192 s. Choices near the threshold still flip between
  rounds. That's written down as open.

The thesis's main idea was **explicit state and access**: a layer between
"what the program means" and "loops", where each read, write and combine is
an op whose implementation (atomic, direct, list, rows, edges, a rank's
message) is *resolved*, not hand-coded per feature. We moved the lowering
onto it in seven steps. The gate for each step: **the LLVM IR of every
program must stay byte-identical**, 394 builds. If it changed, the layer
was the wrong shape. All seven passed.

I expected a smaller compiler afterwards. It got 696 lines bigger. The
layer covers about 2,200 of 22,000 lines, so it couldn't shrink the file
much. What it bought is that a new backend resolves ops instead of threading
special cases through everything. Claude should have said that up front.

## Buffers in pieces

Not everything is an entity. beecs keeps cohorts of bees as rows of a
buffer, a slice per colony. On 19 October a `for` learned to claim rows:

```
for c: Colony, w: mut Cohorts[c.cohorts, Colonies.rows] {
  w[i].bees ...  len(w) ...
}
```

Each colony works on its own window of rows, on any thread or on the GPU.
The compiler checks what it can, and the run catches the rest: two claims
of one row stop the program. Functions can take rows too
(`cohorts: mut Cohorts[]`). The output's md5 was the same on one core, on
all, and on the GPU.

## Bigger than one machine

On 19 October I said I want ent-lang to be appropriate for typical HPC
problems. Agent-based models are the strongest fit, then particles, then
graphs. The next day I set the governing requirement: **worlds bigger than
one machine's memory must be possible.** That means partitions run in
parallel, or one after another from disk, slowly but possibly.

```
distribute Colony by number   // ranges of numbers, one per rank
distribute Bee by home        // each bee where its colony is
```

What got built:

- **D1** (19 Oct): `--ranks N` under MPI. Devices run on rank 0 only.
  Accumulates and running sums go over Allreduce and Exscan. A hundred
  beecs colonies gave the same md5 on 1, 2, 4 and 8 ranks. That's correct,
  and not yet fast: 121 s in one process against 153 s on one rank, and 72
  s on 4 ranks (51 s after fusion worked on ranks).
- **D2**: sends to entities on other ranks, lookups through ghosts, edges
  that change across ranks. That was enough for the epidemic's tracing.
- **Partitions** (20 Oct, in five phases):
  - entities placed where they are spawned;
  - relations kept by each partition's own ends;
  - ghosts *asked for* rather than whole columns gathered;
  - buffers split by rows;
  - several partitions per process, sharing memory, with worlds in files.

The headline numbers, for the epidemic with tracing at a million people:

- **1.15 GB of world ran in a 500 MB limit**, from a file, in 203 s instead
  of 18, to the same bytes.
- **16 partitions in one process** ended in 1.96 GB (4.06 without sharing),
  in 9.4–10.2 s against 9.6 s for one world on all cores.
- In a 2 GB limit, one partition at a time from disk took 172 s, where 16
  processes thrashed for 1,359.

Not everything paid. Asking for ghosts took 28.4 s where gathering whole
columns took 14.3, at four ranks, where whole columns are small anyway.
Buffers split by rows were correct and very slow for pointer-chasing reads:
about 17 rounds of asking a day. Both are in `docs/ranks.md` with their
numbers. Real multi-machine runs haven't happened yet; the laptop is the
cluster.

## Measured, not explained: the clock

One more for the honesty file. Around 1 November the laptop started capping all
cores at 2.0 GHz whenever five or more of them did memory work. A spin loop
ran at 5 GHz. Anything touching L1 dropped to 2.0 at 7 W, far below any
thermal or power limit. It wasn't heat, the power profile, or the charger.
A restart cleared it, and the cause is still unknown. Since then a short
clamp test runs before any all-threads number is trusted. One GPU-against-CPU
number had already gone into a README with the clamp on, and was corrected.

## What this part settled

- **Determinism is a language property,** not a testing aspiration:
  keyed randomness, query-level consistency, ordered combining.
- **Order is declared where it's meaning** (`by`, `in turn by`) and pays
  for itself only there.
- **The serial gaps between parallel loops matter** as much as the loops.
- **Measure the paper's idea before adopting it.**
- **Scale changes speed, never the answer**: threads, ranks, partitions,
  disk.

Next: [08 · The GPU](08-the-gpu.md).
