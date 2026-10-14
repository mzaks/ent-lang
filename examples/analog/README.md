# An analog circuit simulator in ent-lang

Simulates circuits of resistors, capacitors, sources and memristors in
time, node voltage by node voltage, to give the waveforms that
[ngspice](https://ngspice.sourceforge.io/) gives for the same circuit;
then to be faster than ngspice and Xyce on large memristor crossbars by
relaxation, every node solving for its own voltage. `analog-SPEC.md`
says what it is to be and in which milestones. It is being built in
steps; what is here is milestones M0 to M4: a circuit is read, and its
operating point and its waves in time (the trapezoidal rule, steps
chosen by their error) are solved three ways, with memristors of three
models whose states move, and the three ways agree with each other and
with ngspice.

- `prepare.py`: a SPICE netlist made flat into `circuit.net`, the form
  the model reads (spec section 4), and the same flat circuit as a deck
  for ngspice, so both simulate the same elements with the same
  numbers. SPICE's suffixes are worked out as powers of ten (10u is the
  f64 nearest 1e-5), and every number is written with as few digits as
  give the same f64.
- `circuit.ent`: the flat netlist read. Every node is an entity with its
  voltage; ground and every node a source to ground sets are `Fixed`.
  Every device is an entity with its two nodes, a component for its kind
  and two edges of `Touches`. Numbers are read as the f64 nearest them:
  up to 19 digits as a whole number, scaled by the power of ten with
  twice an f64's precision.
- `crossbar.py`: an N by N crossbar of memristors, with or without
  resistance in its wires, its memristors' first states and parameters
  drawn from a seed (spec section 9.2).
- `circuits/`: the small circuits of spec section 9.1: a divider, RC
  low-passes under a step and a sine, an RC ladder of ten stages, a
  memristor under a sine at 1, 10 and 100 Hz, and a memristor in series
  with a resistor, set and reset by pulses; and for the operating point
  an R-2R ladder, a bridge with a current source, and a voltage source
  between two nodes that are not ground; and the two RC low-passes with
  no largest step but the run's (`rc_step_free`, `rc_sine_free`), whose
  steps only their error holds back.
- `devices.ent`: what each kind of device does at a time point, the same
  for every way: each that is no source makes itself straight, its
  `Companion` (the current through it is g * (va - vb) + i0). At the
  operating point a capacitor is open; at a time point h on it is the
  trapezoidal rule's companion, 2C/h and -2q_prev/h - i_prev (backward
  Euler's, C/h and -q_prev/h, on a step that goes by that). A memristor, of each
  model, is made straight at its voltage and state: its current, how
  that grows with the voltage and with the state, and backward Euler's
  equation for the state and how that grows with each. `GMIN` (1e-12 S)
  from every node to ground.
- `sources.ent`: a source's value at a time: DC, SIN, PULSE and PWL as
  SPICE has them.
- `direct.ent`, `direct.c`: way 0. Newton on the whole circuit: the
  companions and the sources go into the matrix of modified nodal
  analysis as entries, which `direct.c` adds up and solves with KLU
  (SuiteSparse); its analysis of the matrix is kept while the pattern
  stays the same. A voltage source between two nodes that are not ground
  is fine here.
- `relax.ent`: ways 1 and 2. Every node not held by a source takes a
  Newton step of its own each round, its neighbours as they stood after
  the round before: v += Sum.i / Sum.g, the current its devices give into
  it over how much that falls as v rises; what a device gives goes to its
  nodes along its edges. Way 1 (Gauss–Jacobi) makes every sum anew and
  steps every node each round. Way 2 goes by events: a node steps only
  where its sum changed, moves only where the step is more than the
  tolerance and then marks its devices, which give their nodes the
  difference in what they give (reactive `for`s, rounds until no node
  told). A node takes even a step smaller than the tolerance, but tells
  only when it has moved more than that since it last told; its devices
  go by the voltages they were told, and its sum says what it took away
  itself (in a transient, steps too small to tell add up over the time
  points, the same way each, and would leave the waves 1% off).
- `steps.ent`: how time goes on (spec 7.2). A step goes by the
  trapezoidal rule, but the first two after the operating point and after
  a corner of a source by backward Euler. Its length is chosen as SPICE
  chooses it: by the error the rule leaves in each capacitor's voltage
  and each memristor's state (h^3/12 times the third derivative, h^2/2
  times the second for backward Euler, from divided differences of the
  last time points), held to TRTOL (7) times the tolerance; no more than
  twice the last step, the circuit's largest step, or the way to the next
  corner (a PULSE's edges, a PWL's points, a SIN's delay), onto which a
  time point is put. A time point whose error is too big is tried again
  shorter, one that does not settle with an eighth of the step.
- `simulate.ent`: the program. It reads "circuit.net" and "run.txt"
  (the way, a seed, whether to write the waves, reltol, vntol, abstol,
  the most rounds, the time of an operating point without `tran`, a
  fixed step and its rule) where it is started; solves the operating
  point and then, for a `tran`, time points to the stop; writes
  "waves.txt" (the time, then each probe, as `%.17g`, a line a time point
  taken) and says how it went: time points taken and tried again.
- `models/`: the memristor models in Verilog-A, the single source of
  each: the HP memristor with Biolek's window (`biolek.va`), VTEAM
  (`vteam.va`, numbers chosen to switch at 1.2 V and not at 0.1 V in the
  crossbars, not a device's fit) and Yakopcic's (`yakopcic.va`, that
  paper's fit of a device). ngspice runs them compiled by OpenVAF
  (`openvaf-r`); `devices.ent` has each by hand, its parameters in the
  order of the file. A state is held at w0 in the operating point; in
  time, way 0 solves for the states with the voltages in each Newton
  step (an unknown each, as ngspice has a model's internal node), and in
  ways 1 and 2 every memristor takes a Newton step of its own on its
  state's equation in each round, beside the nodes (spec 7.3).

```sh
examples/analog/prepare.py all            # the circuits, into build/analog/circuits
examples/analog/crossbar.py 64 1 biolek 0.1 1 write
examples/analog/prepare.py crossbar_64_1_biolek_write
examples/analog/check/parse.py
examples/analog/check/exact.py
examples/analog/check/agree.py
examples/analog/check/spice.py
examples/analog/check/same.py

cd build/analog/circuits/divider          # one, by hand: way 1
echo 1 > run.txt
../../../../tools/ent run -I ../../../../examples/analog ../../../../examples/analog/simulate.ent
```

## What is checked

- `check/parse.py` (check 1): for every prepared circuit, the model has
  as many nodes, devices and probes as the file's second line says, as
  many of each kind as the file has lines of it, ground and the nodes
  that sources to ground set fixed, and two edges for every device, to
  its own two nodes. All 11 prepared so far: the small circuits and
  crossbars of 4 by 4 and 1024 by 1024 (2,099,201 nodes and 3,147,776
  devices, read in 0.7 s with 813 MB). First, 20,000 numbers of every
  size from 1e-300 to 1e300 are each read as the f64 Python reads.
- `check/exact.py` (check 2): for every circuit of up to 80 unknowns,
  the equations of its operating point are solved exactly, in fractions,
  by Python; every probe of way 0 is that to 1e-12 (22 circuits; a
  crossbar's at 50 ms, as its operating point; memristors of Biolek's and
  VTEAM's model, which are resistors at a held state, not Yakopcic's,
  whose current is a sinh). But a
  crossbar whose wires have 0.1 ohm: to 1e-11, since wires of 10 S beside
  memristors of 1e-4 S leave the sense voltages a few parts in 10^12 off
  in f64 (a step of refinement does not help). Then the two RC
  low-passes against their closed forms, at fixed steps: backward
  Euler's error is 1.7e-3 and 3.4e-3 of the range at the circuits'
  steps, and 3.35 and 3.99 times smaller at a quarter of the step (first
  order; rc_step's edge of 1 us falls in another place between time
  points at each step, which is why the ratio is taken over two
  halvings); the trapezoidal rule's on rc_sine 1.3e-4, and 15.6 times
  smaller at a quarter (second order; not on rc_step, whose edge inside
  a step no rule takes to the second order). And with the steps chosen:
  rc_step 9.6e-6 of the range off, rc_sine 1.4e-5.
- `check/spice.py` (check 3): ngspice with tight tolerances, its steps
  no longer than the circuit's, against the model with its steps chosen
  at reltol 1e-5, every probe at every time point within 1e-3 of its
  range: 31 circuits, the memristors of all three models among them
  (compiled from models/ by OpenVAF), crossbars up to 16 by 16 with
  wires and 64 by 64 without. Way 0 on all; ways 1 and 2 on those of up
  to 100 nodes without resistance in the wires. The largest: 5.1e-4 (the
  4 by 4 crossbar of Yakopcic's devices, written); most crossbars 1e-7
  to 2e-4, the single memristors 2e-6 to 2e-5, the RC circuits 2e-5 to
  2.4e-4. (At SPICE's reltol of 1e-3 the error each step may leave adds
  up to more than 1e-3 of a range on some: the Yakopcic crossbar 4.9e-3,
  2.1e-3 at 1e-4.) The divider is 3e-9 below ngspice's: GMIN, see below.
  And a memristor under a sine: its current crosses 0 where its voltage
  does, and the loop's area shrinks from 2.3e-5 V A at 1 Hz to 7.7e-7 at
  10 Hz and 6.7e-8 at 100 Hz. (ngspice takes 20 ms a step with 256
  compiled memristors, so the 64 by 64 crossbars with wires are left
  out.)
- `check/agree.py` (check 4): ways 1 and 2 give way 0's waves to within
  ten times the tolerance at every time point (in fact within a tenth of
  it), on every circuit but the one way 0 alone does: 24 circuits,
  memristors of all three models and crossbars of 4 and 16 by 16 among
  them. Left out unless named, as hours long by relaxation: crossbars of
  64 by 64 (by hand, below), of 16 by 16 with wires whose states move,
  and with wires under 1 ohm, where a pulse's edge takes more than a
  million rounds. A time point may take up to a million rounds here: a crossbar
  with 1 ohm wires takes 130,000 at a pulse's edge.
- `check/same.py` (check 6): the build for one core and the build for
  all of them, on 1, 4, 16 and 24 threads, give the same waves, every
  way, on nine circuits through time, memristors of three models and
  crossbars among them.

### How relaxation goes on crossbars

The operating point of crossbars of Biolek memristors (read: every row
at 0.1 V), at 50 ms; rounds until settled:

| crossbar | wires | way 0 | way 1 | way 2 |
|---|---|---|---|---|
| 4 by 4 | none | 2 | 2 | 2 |
| 4 by 4 | 10 ohm | 2 | 8,664 | 12,577 |
| 4 by 4 | 1 ohm | 2 | 90,636 | 111,449 |
| 4 by 4 | 0.1 ohm | 2 | 947,110 | 982,867 |
| 16 by 16 | none | 2 | 2 | 2 |
| 16 by 16 | 10 ohm | 2 | 17,054 | 22,917 |
| 16 by 16 | 1 ohm | 2 | 161,323 | 188,808 |
| 64 by 64 | none | 2 | 2 | 2 |
| 64 by 64 | 10 ohm | 2 | 87,947 | 115,971 |
| 64 by 64 | 1 ohm | 2 | 253,293 | 288,018 |

On the 64 by 64 crossbar with 1 ohm wires (8,321 nodes) way 0 takes
0.1 s with reading the circuit, way 1 24 s (2.1 billion node steps, 12
ns each) and way 2 154 s (70 ns a step: the events cost more than they
save where every node takes part). One run each, all the cores of the
machine free, a build for one core; not yet a benchmark (that is M5's).
Way 0 on the 1024 by 1024 crossbar with 1 ohm wires (2,100,224
unknowns) takes 382 s and 6.7 GB: the matrix of a grid fills in as KLU
factors it, which a circuit's matrix of the size KLU is made for does
not.
Without resistance in the wires every memristor joins a held driver to
a sense node and the nodes hardly touch each other: two rounds. With it,
every round of Jacobi takes away only a small share of what is left (at
10 ohm 1 in 3,000, at 1 ohm 1 in 30,000), and the rounds grow with the
wires' conductance and with the size. Way 2 saves little: all the nodes
take part in that slow going. This is the central risk the spec names;
Gauss–Seidel along the rows, or solving a whole row line at once and
relaxing between lines, is what it suggests next.

## Not here yet

- Way 0's floating sources are done; the relaxation ways refuse them.
- The deck for Xyce, and `fetch.sh` for the IBM power grids: with M5,
  when Xyce is installed.
- A capacitor's `IC=` is kept, and how it is used (ngspice uses it only
  with `.tran ... uic`) is settled in M2.
- `v(a,b)` probes; a probe is a node's voltage or a device's current.

## Where it goes beyond the spec

- Step control is SPICE's, with one difference that shows: the error is
  worked out from the time points since the last corner of a source only,
  so a step across a corner is not taken as a large error. At SPICE's
  tolerances (reltol 1e-3, trtol 7) the waves of the RC low-passes, with
  nothing but the error to hold the steps back (a largest step of the
  whole run), are 3.2e-3 and 3.3e-3 of the range off their closed forms
  in 35 and 70 time points; ngspice at its own tolerances is 1.1e-2 and
  3.5e-2 off in 38 and 41. The trapezoidal rule waits for three time
  points after a corner, so that its error can be worked out: a step
  taken by it on two lets a memristor's switching through unchecked (the
  4 by 4 crossbar of Yakopcic's devices was 3.6e-3 off ngspice at any
  tolerance).

- When a round of relaxation has settled: SPICE's test (no node steps by
  more than the tolerance, none has more current left) is made for
  Newton, which comes closer fast. Relaxation comes closer by a share of
  what is left, and on a crossbar with resistance in its wires that
  share is tiny: with SPICE's test both ways stopped with the sense
  voltages up to ten times the tolerance off, way 2 more. So here the
  tolerance a step is held to is made smaller by 1 - r, r being how much
  the largest step shrank since the round before (and it only ever gets
  smaller at a time point); then what is left over is within the
  tolerance. It costs about twice the rounds.
- Way 0 has KLU from the start; the dense LU the spec puts first was
  not needed.
- GMIN is from every node to ground, in every way, as the spec says;
  ngspice puts it only beside devices that need it, so the divider's
  output is 2e-9 of itself lower here than there.
- A memristor at the operating point is the resistor its first state
  makes it; "run.txt" says at which time the operating point is taken,
  since a crossbar's pulses are 0 at the start.
- The models hold their state at w0 where `$abstime <= 0`, not where
  `analysis("static")`: OpenVAF's model in ngspice is not told that the
  operating point is static, and started at w = 0.

- `circuit.net` has a line `op` where the netlist asks for the
  operating point (section 4 has only `tran`).
- A memristor is written `X<name> a b <model> name=value ...` in the
  SPICE netlist; the parameters not given take the `.va` file's
  defaults.

## Found in the language on the way, and mended

- A float in a string has six digits after the point, which is for
  people: `devices/decimal.ent` now reads numbers as the nearest f64 and
  writes them as `%.17g` (circuit.ent and the waves use it).
- A `for` over edges could not add into a unique nor into a `var` from
  outside it; now it can (relax.ent's sums go along the edges).
- A line ending in a name with the next line starting with a bracket was
  read as a call; now a bracket that starts a line starts something new.
- A reactive `for` that sends fractions went through every entity each
  time; now it follows its log, visiting the rows it names in order, so
  the fractions add up as before. Way 2 is such a `for`.
