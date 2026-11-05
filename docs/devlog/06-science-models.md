# 06 · Science models, matched with their references

*13 to 24 October 2026: an epidemic, a beehive, traffic, atoms, logic
gates and memristors.*

## The rule

A game can look right. A model has to *be* right, and "it looks like an
epidemic" isn't evidence of anything. So every model in this post came with
a reference: somebody else's program, a published result, or a known exact
answer. The model was not done until it agreed with it. There are two kinds
of agreement:

- **To the bit,** where the arithmetic can be the same. That means the same
  operations in the same order, often against a plain C version written for
  the purpose and built with `-ffp-contract=off`, so that a fused multiply-add
  can't spoil the comparison.
- **In distribution,** where the random numbers differ. Go's PCG stream is
  not ours, so no run of ours is the reference's run. The check then is
  whether our runs spread the way the reference's own runs spread from seed
  to seed.

The second kind has a trap, which the epidemic found for us.

## A city's epidemic (13 October)

The reference is [OpenABM-Covid19](https://github.com/BDI-pathogens/OpenABM-Covid19),
a C model of an epidemic person by person. I wrote a spec with milestones
E1 to E6 and five checks. E1 to E5 were built on 13 October. The model has
the illness by stage and age, households, five networks of ties at work and
school, meetings by chance, a lockdown, and then the measures: staying home
with symptoms, tests, and an app that tells those you met.

A person's illness is plain data, and a system moves on everyone whose day
has come:

```
system progress() {
  for e, p: Person, a: Age, d: mut Disease where d.at == Clock.day {
    let stage = d.next
    let k = key(Run.seed, p.id, Clock.day, STEP)
    let next = next_stage(Ages, stage, d.path, a.group, uniform(k, 0))
    d.stage = stage
    ...
```

What is checked:

- `check/disease.py` infects 100,000 people and holds every step of every
  illness against the tables: 101 numbers, each within four standard
  errors.
- `check/city.py` holds the city against the reference's targets: 52
  numbers covering ages, households, ties per member and meetings per day.
- `check/curves.py` runs both models for 20 seeds each from one parameter
  file. Each day's median of new infections must fall inside the
  reference's 5% to 95% band. The peak's day, its height and the total are
  compared with a Kolmogorov–Smirnov test. Then it does all of that again
  with each measure switched on in turn.

The speed came almost for free. A day of a million people takes 89.8 ms on
one core where the reference takes 590, in about a fifth of its memory. Ten
million people take 1.6 s a day. The reference would need some 38 GB for
that, and the laptop has 22. One trick helped a lot: whom you met by chance
is never stored. The pairing of stubs is an invertible permutation worked
out from the seed and the day. So "whom did this person meet last Tuesday"
is recomputed when tracing needs it, not looked up in a diary. That is eight
numbers per person where the reference keeps every meeting for ten days.

**Lesson 1: chance is a suspect too.** Over seeds 1 to 60, the peak came
out 0.9% lower than the reference's (KS p ≈ 0.004). That cost an hour of
hunting. Seeds 61 to 120 agreed, and all 120 together agree. The reference's
own batches differ that much. So now, before hunting a gap under 1%, we
replicate it on fresh seeds.

**Lesson 2: instrument both and diff, don't reason.** With tracing on, 5%
of the traces went missing. The reference handles a day's events one after
another: symptoms, then tests, then releases. Our per-person flags let a
later event overwrite an earlier one on the same day. Hours of reasoning
didn't find it. Putting the same counters into a private copy of the
reference and into the model found it in an hour.

The model also asked the compiler for things, and got them on the same day.
Whole-number sums into a unique became reductions in a parallel `for`, and
a `for` may now send from a loop inside its body. Post 07 is about why that
matters.

## A honeybee colony (14 October)

[beecs](https://github.com/mlange-42/beecs) is BEEHAVE's model of a hive
through the year, written in Go: brood cohorts, foragers in squadrons,
patches of flowers, Berlin's weather in 2000. It was ported the day I asked
for it. `check/curves.py` holds the colony's year against beecs's spread
over 400 seeds, in six scenarios.

The first run with each forager as its own entity was killed after two
minutes. The cause was `for ... in turn by`, which lets entities act one
after another in key order, as beecs's squadrons do. It put each entity in
place by insertion, which is quadratic when the keys come in random order.
The fix sorted the entities once (7.3 s), then used a radix sort that skips
bytes all keys share (1.83 s), then made smaller fixes in the model (1.40 s).
Go took 1.55 s. A later variant without turns, where squadrons claim loads
from a patch all at once and share what there is, got down to 0.64 s after
profiling, against Go's 1.50.

Then it got a window: charts, weather, a "why did they forage" box, and
knobs to turn while it runs. Running a hundred colonies at once on the GPU
is in [post 08](08-the-gpu.md).

## Traffic (22 to 24 October)

Traffic has the nicest references, because some of them are exact:

- **Nagel–Schreckenberg** cars in cells on a ring. Without braking, the
  flow is exactly `min(density · vmax, 1 − density)`; with vmax 1 that is
  Wolfram's rule 184. With random braking there is a closed formula for an
  endless road, and roads of 1,000 cells came within 1.4% of it.
- **The intelligent driver model** in f32. It matches a plain C version
  to the bit: every car's place and speed, every step.
- **Sugiyama's experiment** (2008): 22 cars on a 230 m circle. A jam
  forms that nothing caused and travels backwards at 15.3 km/h. The real
  drivers' jam went back at 15 to 20.
- **Lanes with MOBIL** lane changes, trucks and impatient drivers: bitwise
  equal to `check/lanes.c`, lane changes and all.

Then came junctions, in the style of traffic-simulation.de: lights, a
roundabout, a priority road, all-way stops, on-ramps and off-ramps, a
two-lane roundabout. These have no reference, so the checks are physical:
zero overlaps and zero close pairs in ten simulated minutes per scene, and
the same on one core and on all. Capacities were measured rather than
matched. At 600 cars an hour per street, the roundabout does about 1,840
an hour, lights and priority about 1,140, and the all-way stop 860.

The two-lane roundabout gridlocked three different ways before it worked.
One fix: a car crossing lanes may enter first, unless the other car is 8 m
nearer. When this rule was absolute, both cars yielded to each other
forever. It was politeness as a deadlock.

Keeping the cars in order by lane and position every step is what led to
indexes in the language: [post 09](09-indexes.md).

## Atoms: LAMMPS's LJ melt (22 October)

32,000 atoms on an fcc lattice melt under the Lennard-Jones potential, as
in LAMMPS's own benchmark. At step 0, the energy per atom (−6.7733681) and
the pressure equal LAMMPS's output and the lattice sums worked out by hand.
The whole run is bitwise equal to a plain C version, with half lists and
with full ones.

Then it had to be fast, which was humbling. The first version scanned 27
cells every step and took 4.76 s on one core, where LAMMPS took 0.74. We
added Verlet neighbour lists with a skin (as LAMMPS does), half lists, and
fewer pairs that need wrapping. Neighbour search learned to prune cells and
to go through rows in blocks of 64. One core now takes 0.90 s, against
plain C's 1.10 and LAMMPS's 0.74. On all threads it takes 0.17 s, against
LAMMPS's 0.12 on 12. That's close, and it's still behind, and the README
says so.

## Logic gates (13 October)

A gate-level simulator: circuits as and-inverter graphs, one clock, 64 runs
side by side in each u64. All 49 test circuits (ISCAS-85/89, EPFL) give
Icarus Verilog's bits. PicoRV32, a RISC-V core of 30,383 ANDs, runs
Dhrystone to the end in 583,031 cycles, identical under Icarus, Verilator
and three ways of the model.

The speed against Verilator had a moral:

- With the netlist written out as ent-lang code, an AND a line, the model
  is **1.8× as fast as Verilator on the same gates**, and it builds faster.
- Verilator given the processor's *Verilog* is 16× faster again. It adds two
  32-bit numbers in one instruction where the gates need hundreds of ANDs.
- So we generated ent-lang from Yosys's word-level cells instead: 611
  operations in place of 30,383 ANDs. The model's cycle went from 4.28 µs to
  0.087, against Verilator's 0.071.

It was the gates that cost, not the language.

## Memristors (14 and 15 October)

An analog simulator of resistors, capacitors, sources and memristors, held
against ngspice and Xyce. It has three memristor models compiled by OpenVAF,
crossbars up to 1024×1024, and the IBM power grids. It has three ways to
solve:

- a direct sparse solve (KLU, from a C device), which is way 0;
- two ways by relaxation, where each node solves for its own voltage.

The results go both ways:

- Way 0 reads a 256-crossbar in 200 s where Xyce takes 240 and ngspice 470.
  It solves the ibmpg2 grid in 0.53 s against 3.6 and 6.3, and is as near
  the published solution as they are (5 to 6 µV).
- Relaxation wins only when the wires have no resistance: 1.7 s on four
  cores against Xyce's 20.
- Give the wires resistance and relaxation needs up to 100,000 rounds per
  time point. ibmpg2's operating point never settles.

The work is marked as in progress at M5. The results file has a column
called "off" for runs whose waves strayed more than 1% of a probe's range,
and ngspice is in that column too.

## And briefly

The spiking network from [post 03](03-from-dialect-to-language.md) was held
against like-for-like C before any of these. Every model since has been held
against something.

## What the models taught

- **A reference turns "looks right" into a number.** Exact formulas first,
  bitwise C next, statistics last.
- **Replicate a small gap on fresh seeds** before hunting it.
- **Instrument both sides identically and diff.** It beats thinking.
- **Every model found compiler work.** Sums as reductions, sends from loops,
  a sort for `in turn`, reactive `for`s that follow their logs, and a
  parser that hung on a line 32 operators deep. The models were never just
  demos. They were the test suite of the design.

Next: [07 · All the cores, deterministically](07-all-the-cores.md).
