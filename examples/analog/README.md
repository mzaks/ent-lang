# An analog circuit simulator in ent-lang

Simulates circuits of resistors, capacitors, sources and memristors in
time, node voltage by node voltage, to give the waveforms that
[ngspice](https://ngspice.sourceforge.io/) gives for the same circuit;
then to be faster than ngspice and Xyce on large memristor crossbars by
relaxation, every node solving for its own voltage. `analog-SPEC.md`
says what it is to be and in which milestones. It is being built in
steps, and still in progress: M5 is not finished (see "Not here yet").
What is here is milestones M0 to M5: a circuit is read, and its
operating point and its waves in time (the trapezoidal rule, steps
chosen by their error) are solved three ways, with memristors of three
models whose states move, and the three ways agree with each other, with
ngspice and with Xyce; the IBM power grids come out as published; and
`bench/analog` times it all against ngspice and Xyce.

- `prepare.py`: a SPICE netlist made flat into `circuit.net`, the form
  the model reads (spec section 4), and the same flat circuit as a deck
  for ngspice and one for Xyce (`circuit.xyce`, where Xyce has the
  memristors' model built in: Yakopcic's), so all simulate the same
  elements with the same numbers. A voltage source of 0 V DC whose
  current is not probed (a via of a power grid, a crossbar's row held at
  0) makes its two nodes one (`merged.txt` says which names went where),
  but for ngspice, which is given the sources back (at tight tolerances
  its steps shrink to nothing where a device switches in a crossbar
  whose rows are made ground: 600 s where with them it takes 2);
  a circuit with no probes and no `tran` shows every node. SPICE's suffixes are worked out as powers of ten (10u is the
  f64 nearest 1e-5), and every number is written with as few digits as
  give the same f64.
- `circuit.ent`: the flat netlist read. Every node is an entity with its
  voltage; ground and every node a source to ground sets are `Fixed`.
  Every device is an entity with its two nodes, a component for its kind
  and two edges of `Touches`. Numbers are read as the f64 nearest them:
  up to 19 digits as a whole number, scaled by the power of ten with
  twice an f64's precision.
- `fetch.sh`: the IBM power grids ibmpg1 to ibmpg6 (Nassif 2008) and
  their published solutions, from the mirror at UC Santa Barbara, into
  `build/analog`.
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
  shorter, one that does not settle with an eighth of the step. One in
  which a memristor's state moved more than 0.25 (the states here are of
  ranges about 1) is tried again with an eighth of the step by backward
  Euler: the trapezoidal rule takes half a step from the rate at the
  last point, and where that is a window turning a state back from past
  its end it throws the state far, an RRAM cell's rung past zmin to z =
  1.97, in a step so short that the error worked out lets it be (0.35%
  of its tolerance).
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
  crossbars, not a device's fit), Yakopcic's (`yakopcic.va`, that
  paper's fit of a device) and IHP's HfO2 RRAM cell (`rram.va`: the
  enhanced Stanford-PKU model of Al Beattie et al., IEEE TNANO 2023,
  with its Table I; multilevel by the gate voltage `ug`; four
  departures so an implicit step holds it, said in the file: the field's
  threshold a logistic 0.7% of it wide, the rate capped at 1e8/s, a
  window at the state's ends, and the rate's gap taken within them: past
  z = 1.2 the paper's gamma is below 0, its field step closes, and a
  state a step threw there stayed for good, its cell dead). Swept as that paper's Fig. 6
  (`circuits/rram_ug1`, `rram_ug12`, `rram_ug16`) it sets at 0.63 V and
  gives the figure's model curves at ug 1 and 1.2 V; at 1.6 V the
  figure's low resistance is that of 1 V, which its own formula does
  not give, and here it is 2.2 times lower. ngspice runs them compiled by OpenVAF
  (`openvaf-r`); `devices.ent` has each by hand, its parameters in the
  order of the file. A state is held at w0 in the operating point; in
  time, way 0 solves for the states with the voltages in each Newton
  step (an unknown each, as ngspice has a model's internal node), and in
  ways 1 and 2 every memristor takes a Newton step of its own on its
  state's equation in each round, beside the nodes (spec 7.3). A state
  that goes on moving for 50 rounds while its voltage holds still is
  stuck in its own Newton steps (at the RRAM cell's set, with too long a
  step, they go round in a cycle), and the time point is tried again
  shorter, as way 0 does after 50 rounds, not after a million.
- The parts of the guided axon growth circuit of Jenderny, Ochs and Xue
  (Sci Rep 14:16643, 2024), each a model in `models/` and by hand in
  `devices.ent` as the memristors are, a state each: a Morris-Lecar
  oscillator's ion channels (`ml.va`, its potassium gate the state;
  beside a capacitor, a membrane), a synapse (`synapse.va`), the
  memsensor's growing memristor (`growth.va`, the Heaviside steps of
  its rate a smoothstep 1 mV wide: a logistic leaks, and its tail beats
  the retention), and its pruning-cue sensor (`prune.va`): a resistor
  whose state is the cue it senses, made by two more nodes it does not
  touch, two targets' membranes (`Sensing`: in a netlist line `M a b
  model c d numbers`; way 0 puts the state's dependence in those nodes'
  columns). Each target's cue is linear in its own with one rate of
  decay, so what a sensor senses, its share of each, is too, and one
  state holds it exactly; the minimal example's second is ground.
  The paper's printed values and where they are read otherwise are in
  the files (Rp1 1.25 MOhm, not the table's 125 kOhm, since its text has
  Rp1 above M1). Table 1's oscillator rests at -69.4 mV, near EL as the
  paper has it, but an operating point from 0 V lands at -26.5 mV, its
  other root, where the node's conductance is -63 uS: no rest, and every
  oscillator fires once by itself and grows whatever memsensor senses a
  gradient. So the circuits hold their membranes at rest in the
  operating point (`.ic`, as SPICE does without uic: 1e10 S to the
  value, SPICE3's, then let go; way 0's, and ngspice's). The
  Morris-Lecar node's sodium conductance is negative
  under its threshold, so a node's conductance can be 0 or less, and
  ways 1 and 2, whose step is a node's current over it, would go away
  from the root: they say so and do not do such a circuit.
- `axon.py`: the paper's minimal example (Fig. 3 and 4) as a netlist,
  a 2 by 2 grid of oscillators, the start fed pulses, two memsensors
  growing from it and two synapses on to the target; `circuits/axon_grow`
  (50 ms) is it, held to ngspice. Run for 5 s (`check/axon.py`, 17 s,
  170,000 time points) the memsensors grow from 800 to 20 kOhm in 10 ms;
  the one whose synapse the target's cue reaches most, Ma1, goes to 2.5
  MOhm by 0.5 s and its memristor back to 840 kOhm by 4.3 s, 980 by 5;
  Ma2 is kept, at 120 to 140 kOhm (the paper: under 100), and the pruned
  synapse's node stops spiking. Which synapse is pruned hangs on where
  eq. 4b's vectors are taken from; at the nodes a memsensor grows to, as
  here, it is the paper's.
  The paper's biologically inspired example (Fig. 5), `axon.py ...
  grid`: 15 by 15 oscillators, 416 memsensors and four synapses, two
  neurons growing to two targets, the paths Fig. 5a draws (its growth
  cue map is shown, not given: the memsensors along them sense a
  gradient of 5, the others none, which gives the paper's 800 kOhm and
  1.8 MOhm to start from; N1 at (3,15), where the figure has it, not
  (5,15), the text's). Run for 5 s (`check/axon.py grid`, 12.5 minutes,
  442,000 time points, 1.9 GB of waves read as they come) the paths grow;
  the two memsensors into the synapses that should not be go to 2.5
  MOhm by 0.5 s and 3.2 by 5, their memristors back over 800 kOhm, and
  the ones before them on those paths over 100 kOhm about where Fig. 5a
  shows them gone (and the last but one of N1's branch, which it
  keeps); the paths to be kept stay under 122 kOhm (the paper: 100: the
  one into N1''s synapse hears a quarter of its cue, as Ma2 does), and
  both targets fire to the end. Here the growth is done in 5 ms, where
  the paper's takes 0.22 s, a pulse period a hop: as Table 2 is read (S
  4 kHz) a memristor sets in 0.25 ms over U+, within the spike that
  reaches it, where the paper's Fig. 4b has 0.6 of it in the first
  pulse's 2 ms.
  With `rram` (`axon.py ... rram [shift=s] [sigma=s] [seed=n]`) the
  growing memristors are IHP's HfO2 cells (`rram.va`, from their high
  resistance, gate at 1 V) and the neurons' voltages ten times the
  paper's, so that a spike, 1.1 V across a memsensor, passes a cell's
  0.63 V set (the currents ten times too, the times the same). The cells
  hold their state, so a connection is pruned by its cue sensor alone.
  `shift` moves the cells' g0, uth, Emin and v0 all one way (0.1: 10%
  up), `sigma` draws each cell's about that from a normal. The minimal
  example ends as the paper's in 10 of 10 runs (shifts of 10% either
  way; sigma 10% and 20%, four seeds each), one late in growing. The
  grid, 5 s (14 minutes each, three at once):

  | cells          | the network at 5 s                        | on time |
  |----------------|-------------------------------------------|---------|
  | as measured    | the paper's (kept at most 110 kOhm)       | yes     |
  | 10% easier     | the paper's                               | yes     |
  | 10% harder     | the paper's                               | grown by 2 s, not 0.22 |
  | sigma 10%, 2 seeds | the paper's on both                   | some hops late |
  | sigma 15%, 5 seeds | the paper's on 2 (seeds 2, 4)         | some hops late |
  | sigma 20%, 5 seeds | the paper's on 1 (seed 4)             | some hops late |

  (These counts are with way 0's thrown states caught, below; before,
  the same counts came of another cause: every path that failed began at
  a cell of small g0, which way 0 threw past its end, dead, where
  ngspice holds it set.) Where it is not, a cell partly set stops: as a
  hop grows the neuron after it fires with the one before, the voltage
  across the memsensor falls under the cell's set, and the cell stays
  where it got (as the kept ones do at 110 to 120 kOhm, there at 120 to
  530 kOhm, from 0.25 s on; such a cell alone on the minimal example
  sets fully, ours and ngspice's alike). Its target then fires less (N1'
  2 to 4 times in the last 60 ms, not 7), its cue does not reach the
  false synapse's sensor, and that synapse stays (at 37 to 435 kOhm);
  on two runs of 20% both targets fall silent. One cell of g0 0.48 sets
  to 38 times the low resistance and its path does not carry a spike.

  How fast a cell sets hangs on how far a spike goes past its threshold:
  cells 10% harder set a little each spike and the growth goes a hop in
  50 to 100 ms, about the paper's pace (above); a cell that a spike does
  not reach at all holds its path open for good. `check/axon.py rram`
  holds such runs to the network at the end and says the rest as late.

```sh
examples/analog/prepare.py all            # the circuits, into build/analog/circuits
examples/analog/crossbar.py 64 1 biolek 0.1 1 write
examples/analog/prepare.py crossbar_64_1_biolek_write
examples/analog/check/parse.py
examples/analog/check/exact.py
examples/analog/check/agree.py
examples/analog/check/spice.py
examples/analog/check/same.py
examples/analog/check/xyce.py             # crossbars of Yakopcic's devices
examples/analog/fetch.sh && examples/analog/prepare.py ibmpg1 ibmpg2 ibmpg3 ibmpg4 ibmpg5 ibmpg6
examples/analog/check/grid.py ways=0
python3 bench/analog/run.py               # bench/analog/RESULTS.md

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
  range: 47 circuits (3 crossbars of Yakopcic's devices left out:
  ngspice not done in 600 s), the memristors of all four models among
  them (compiled from models/ by OpenVAF), crossbars up to 16 by 16 with
  wires and 64 by 64 without. Way 0 on all; ways 1 and 2 on those of up
  to 100 nodes without resistance in the wires. The largest: 5.1e-4 (the
  4 by 4 crossbar of Yakopcic's devices, written); most crossbars 1e-7
  to 2e-4, the single memristors 2e-6 to 2e-5, the RC circuits 2e-5 to
  2.4e-4. (At SPICE's reltol of 1e-3 the error each step may leave adds
  up to more than 1e-3 of a range on some: the Yakopcic crossbar 4.9e-3,
  2.1e-3 at 1e-4.) The divider is 3e-9 below ngspice's: GMIN, see below.
  A point on a jump (`check/common.py`: a probe moving one way at least
  5% of its range at a pace that crosses all of it in 1e-2 of the run or
  less: a switching device's runaway, a spike's edge)
  is held to when the jump comes instead: the RRAM cell's set takes its
  current up tenfold in nanoseconds, and comes 7e-5 s early at reltol
  1e-5, 2.8e-5 at 1e-7, against a stiff solver's at 1e-12 (of a 4 s
  sweep), so a point there is off by a tenth of the range at any
  tolerance. From 1e-3 of the run before such a jump to 1e-4 after, a
  point off is to cross the jump's middle within 1e-4 of the run of
  ngspice and to be within its tolerance of what ngspice goes through
  over as long as the jump is early or late. The three RRAM sweeps: 38
  to 43 such points, the rest within 8.7e-4 to 1.0e-3 of the range.
  And a memristor under a sine: its current crosses 0 where its voltage
  does, and the loop's area shrinks from 2.3e-5 V A at 1 Hz to 7.7e-7 at
  10 Hz and 6.7e-8 at 100 Hz. (ngspice takes 20 ms a step with 256
  compiled memristors, so the 64 by 64 crossbars with wires are left
  out.)
- `check/axon.py`: the minimal example of guided axon growth, 5 s, held
  to the paper's Fig. 4 (`axon.py` above): grown under 100 kOhm by 10
  ms, Ma1 over 1 MOhm from 0.5 s and its memristor over 800 kOhm by 4.3
  s, Ma2 under 150 kOhm (said against the paper's 100), and the pruned
  synapse's node silent in the last 60 ms while the others spike. With
  `grid`, the 15 by 15 example to Fig. 5: the paths grown by 0.25 s,
  those to be kept under 150 kOhm from then (said against 100), the
  memsensors into the synapses not to be over 1 MOhm from 1 s and their
  memristors back over 800 kOhm from 4.5 s, and both targets firing at
  the end.
- `check/agree.py` (check 4): ways 1 and 2 give way 0's waves to within
  ten times the tolerance at every time point (in fact within a tenth of
  it), on every circuit but the one way 0 alone does: 32 circuits,
  memristors of all four models and crossbars of 4 and 16 by 16 among
  them; the Morris-Lecar circuits (`ml_pulse`, `ml_synapse`, `axon_grow`)
  are way 0's alone (`models/ml.va`'s negative conductance, above; and
  `.ic`, which ways 1 and 2 do not do).
  Left out unless named, as hours long by relaxation: crossbars of
  64 by 64 (by hand, below), of 16 by 16 with wires whose states move,
  and with wires under 1 ohm, where a pulse's edge takes more than a
  million rounds. A time point may take up to a million rounds here: a crossbar
  with 1 ohm wires takes 130,000 at a pulse's edge. A point on a jump is
  held to when it comes, as in check 3: the RRAM sweeps' ways 1 and 2
  set 9.7e-5 s after way 0 at reltol 1e-3 (10 to 14 points), and are
  within half the tolerance elsewhere.
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

- M5 is not finished. The spice check (check 3) was last run in full
  before ngspice was given the 0 V sources back; since then 25 circuits
  agreed with it and it was stopped. Three crossbars written, of
  Yakopcic's devices, that M4 did not have (16 by 16 with 0 and 10 ohm
  wires, 256 by 256 with none) ngspice does not finish in 600 s at its
  tight tolerances, even with the sources; check 3 says so and leaves
  them out. The written crossbars do not hold to 1e-2 of a range in the
  benchmarks, Xyce included (bench/analog/RESULTS.md), and the 256 ones
  with wires are past 600 s for all.
- The axon growth circuits with RRAM cells: how much spread they bear
  (all of 2 seeds at 10%, 2 of 5 at 15%, 1 of 5 at 20%), what would
  make a path bear a cell that does not set, and the cells' reset,
  which the circuit never gives them (a pruned cell stays set). Growth a
  pulse period a hop, as the paper's (above). The 5 s minimal
  example tries a fifth of its time points again, the grid three in
  ten: Newton's steps there come to
  what is not a number somewhere (where, not yet found; SPICE limits
  how far a step moves a node, this does not). Its current probes are
  not held to ngspice: where a growing memristor speeds up, ngspice at
  its tightest is 1.3e-3 to 5e-3 of the range off where ours converge
  (`axon.py` says more). From rest (`.ic`) ngspice's trapezoidal rule
  stops at a growing memristor's first switch (29 us; at reltol 1e-5 it
  does not): check 3 gives it Gear's rule there, whose reltol 1e-6 is
  its tightest (at 1e-7 it stops too), 1.2e-3 of the range off where
  ours converge; axon_grow holds, 4,400 of its points on jumps, at their
  times. The 15 by 15 grid (1,474 nodes) is past check 3's 600.
- Way 0's floating sources are done; the relaxation ways refuse them.
- Xyce has no Biolek and no VTEAM built in, and its Verilog-A plugins
  need ADMS, which the package installed has not: check 5 and the
  benchmarks are of Yakopcic's devices only. Xyce in parallel needs a
  build with MPI.
- The transient IBM grids (ibmpg1t to ibmpg6t).
- A capacitor's `IC=` is kept, and how it is used (ngspice uses it only
  with `.tran ... uic`) is settled in M2.
- `v(a,b)` probes; a probe is a node's voltage or a device's current.

## Where it goes beyond the spec

- The first step after the operating point or a corner, which no error
  holds, is shorter by the square root of reltol under 1e-3: backward
  Euler's error goes as its square, and what it leaves a Morris-Lecar
  membrane at rest under its threshold grows (0.1 us early of 15 at the
  first spike, at any tolerance). And a circuit may ask for a tighter
  vntol (`.options vntol=`, the run's is the larger): the axon circuits
  1e-9, where at the run's 1e-6 the spikes come 0.1 us early, as ngspice
  has them only with its own vntol of 1e-12. What is not a number never
  settles, in Newton's steps or in relaxation, and the time point is
  tried again shorter (a 5 s axon run took NaN from 0.42 s on as
  settled).
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
