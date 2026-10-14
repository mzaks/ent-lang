# Spec: an analog circuit simulator in ent-lang

*For `examples/analog/`. The sibling of `examples/logic/`: same layout,
same habits (a prepare step, numbered checks that the check scripts
name, benchmarks that verify before they time).*

## 1. Goal

Simulate analog circuits with memristors in time, node voltage by node
voltage, and give the waveforms that ngspice gives for the same
circuit, models and inputs, within stated tolerances. Then be faster
than ngspice and Xyce on large memristor crossbars.

The main method is **relaxation** (iterated timing analysis): every
node solves for its own voltage with a local Newton step while its
neighbours stand still, and notifies its neighbours when it moves. This
is the analog counterpart of `examples/logic/events.ent`. A direct
solver is kept alongside it as the in-repo reference and as the
fallback for tightly coupled circuits.

The question this example is meant to answer: **for which circuits does
relaxation in ent-lang converge, and when it does, how fast is it
compared to a direct sparse solve?** Crossbars with tunable wire
resistance are the main experiment, because wire resistance sets how
tightly the circuit is coupled.

## 2. Scope

### In

- Two-terminal elements: resistor `R`, capacitor `C`, independent
  voltage source `V` and current source `I`, memristor `X…` with a
  model.
- Source waveforms: `DC`, `SIN`, `PULSE`, `PWL`.
- Analyses: DC operating point (`.op`) and transient (`.tran`).
- Memristor models: Biolek (HP model with Biolek window) first, then
  VTEAM, then Yakopcic. Other models later via §8.4.
- `f64` everywhere.

### Not here (for now)

- Inductors, controlled sources, diodes, MOSFETs, any BSIM model.
- AC, noise, sensitivity and pole-zero analyses (they need complex
  arithmetic, see §13).
- Subcircuits beyond what the prepare step flattens.
- Voltage sources between two non-ground nodes in the relaxation ways
  (the direct way handles them; see §6.4).
- Mixed-signal co-simulation with `examples/logic` (see §12, M7).

## 3. Layout

Mirroring `examples/logic/`:

| File | Contents |
|---|---|
| `README.md` | What is here, how to run it, what is checked, what is not here (same style as logic's). |
| `fetch.sh` | Fetches external benchmark circuits into `build/analog/` (not in the repo). |
| `prepare.py` | Turns a SPICE netlist into the flat netlist the model reads (§4), plus the ngspice deck and the Xyce deck for the same circuit. |
| `crossbar.py` | Generates N×N crossbar netlists (§9.2). |
| `circuit.ent` | Reads the flat netlist; spawns nodes, devices and edges. |
| `devices.ent` | Device models: what each device contributes to its nodes. |
| `memristor.ent` | Memristor models and their state update. |
| `sources.ent` | Source waveforms as fns of time. |
| `direct.ent` + `direct.c` (or `.build`) | Way 0: assemble the matrix, solve it in C (§6.2). |
| `jacobi.ent` | Way 1: Gauss–Jacobi relaxation, all nodes every round (§6.3). |
| `events.ent` | Way 2: event-driven relaxation, only nodes that were told (§6.3). |
| `step.ent` | Time stepping, step acceptance, step control (§7). |
| `simulate.ent` | The program: reads `circuit.net` and `run.txt`, writes `waves.txt`. |
| `view.ent` | Later: a small circuit in a window, node voltages as colours, an I–V trace of one memristor. |
| `models/*.va` | The Verilog-A source of each memristor model, the single source of truth (§8). |
| `check/*.py` | The checks of §10, each docstring naming its check number. |
| `bench/analog/run.py`, `RESULTS.md` | §11. |

## 4. Input: the flat netlist

`prepare.py` reads a SPICE netlist (the subset in §2), flattens it and
writes `circuit.net`, a line-oriented format that is easy to read in
ent-lang (the role AIGER plays for the logic simulator):

```
analog 1                         # format version
nodes <n> devices <d> probes <p>
node <index> <name>              # index 0 is ground; one line per node
R <a> <b> <ohms>
C <a> <b> <farads> <v0>
V <a> <b> <kind> <params...>     # kind: dc | sin | pulse | pwl
I <a> <b> <kind> <params...>
M <a> <b> <model> <params...>    # memristor; params in the order models/<model>.va declares them, then w0
probe v <node>                   # what goes into waves.txt
probe i <device-index>
tran <tstep> <tstop> [<tmax>]
```

- Node and device indices are 0-based, dense, in file order.
- Numbers are plain decimal or exponent notation; `prepare.py` expands
  SPICE suffixes (`k`, `meg`, `u`, `n`, `p`, …) so ent-lang never sees
  them.
- `PWL` points go into a buffer (`capacity` set by `simulate.ent`), the
  source holding its first row and count.
- `prepare.py` also writes `circuit.cir` for ngspice and `circuit.xyce`
  for Xyce from the same flattened circuit, so all three simulate the
  identical circuit (the role of Yosys in `logic/prepare.py`).

`run.txt` (same idea as the logic simulator's): `way`, `seed`,
`write-waves` (0/1), and optional overrides for `reltol`, `vntol`,
`abstol`, `max-rounds`. Missing values have defaults (§7).

`waves.txt`: one line per accepted time point: the time, then each
probe's value, space-separated, `%.17g` so nothing is lost when compared.

## 5. Model in ent-lang

### 5.1 Entities

- `Node { v: f64, v_prev: f64 }`, the voltage now and at the last
  accepted time point. Ground is a node with `Fixed`. A node whose
  voltage a grounded source sets also has `Fixed` (its `v` is written by
  the source each step, never solved).
- `Sum { i: f64, g: f64 }` on every node: the device currents into the
  node and their derivative with respect to the node's own voltage,
  combined by `apply ... add` each round.
- Devices: one entity each, `Two { a: entity, b: entity }` for its
  terminals, plus `Resistor { g }`, `Capacitor { c, q_prev, i_prev }`,
  `Source { … }`, `Memristor { model params…, w, w_prev }`.
- Relation `Touches` from device to node (two edges per device), so a
  node that moved can tell its devices, and a device can tell its
  nodes: the `Feeds` of the analog side.

### 5.2 Determinism

`apply ... add` combines in a fixed order, so sums of currents are the
same on one core and on many. This must hold for every way (check 6),
the same guarantee the logic and epidemic examples give.

## 6. Solving a time point

At a time point every device is linearised around the current guess
(companion models: a capacitor under backward Euler becomes
`g = C/h` in parallel with a current source; a memristor becomes its
small-signal conductance plus the current offset). All three ways solve
the same nonlinear equations, KCL at every non-fixed node; they differ
only in how.

### 6.1 What a device contributes

For each device and each of its terminals: the current into the node and
`∂i/∂v` of that node's own voltage. For the direct way, also the
off-diagonal entries. This is one function per device kind, shared by
all ways (`devices.ent`), so the ways cannot drift apart in physics.

### 6.2 Way 0: direct (reference and fallback)

Newton–Raphson on the whole circuit: assemble the MNA matrix and
right-hand side (devices `apply`/stamp into coefficient entities or a
buffer in a fixed order), solve in C, update all node voltages, repeat
until converged (§7.1).

- The C side: a dense LU for small circuits first (no dependencies), then
  KLU (SuiteSparse) behind the same interface, via an `extern system`
  or a device module with `.build`.
- Handles floating voltage sources (they add a branch-current unknown).
- This is what checks 4 and 5 compare the relaxation ways against
  inside the repo, before going to ngspice.

### 6.3 Ways 1 and 2: relaxation

Per round, every non-fixed node takes one scalar Newton step with its
neighbours held at their values from the round before:

```
v_new = v - Sum.i / Sum.g        (Sum.i: net current into the node)
```

- **Way 1, Gauss–Jacobi:** every node, every round. Reads neighbours'
  values from the previous round (an `old` column, or an explicit
  `v_round` field), so all nodes can update in parallel.
- **Way 2, event-driven:** a node recomputes only if a neighbour moved by
  more than the tolerance in the last round; a node that moves tells its
  devices' other nodes (reactive `for`s along `Touches`, rounds until
  nothing is told), as in `logic/events.ent`.
- Damping: limit `|v_new - v|` per round (a parameter; default no
  limit), to be tuned by check 5.
- Count rounds per time point and node updates per time point; report
  them (the analog of logic's "ANDs worked out n times in m rounds").
- If a time point does not converge within `max-rounds`, the step is
  rejected and halved (§7.2). Never silently accept.

### 6.4 Limits of relaxation (stated, not hidden)

- Every node needs a conductance to something (`Sum.g > 0`). Add `gmin`
  (default 1e-12 S) from every node to ground, as SPICE does, in all ways
  including the direct one, so they solve the same equations.
- Floating voltage sources: a circuit with one runs only under way 0; the
  relaxation ways refuse it with a message.
- Tightly coupled circuits converge slowly or not at all. That is a
  result to measure (§11), not a bug to hide.

## 7. Time stepping

### 7.1 Convergence of a time point (all ways)

SPICE's criteria, so tolerances mean the same as in ngspice:

- `|Δv| ≤ reltol·max(|v|, |v_prev_iter|) + vntol` for every node, and
- `|net current| ≤ reltol·(largest device current at the node) + abstol`.

Defaults: `reltol = 1e-3`, `vntol = 1e-6 V`, `abstol = 1e-12 A`.

### 7.2 Integration and step control

- Milestone 2: backward Euler, fixed step `tstep`.
- Milestone 4: trapezoidal rule, step chosen by local truncation error
  of the capacitor charges and memristor states (as SPICE does), bounded
  by `tmax` and by the next source breakpoint (`PULSE` edges, `PWL`
  corners), with rejection and halving on non-convergence.
- `waves.txt` holds accepted points only.

### 7.3 Memristor state

The state `w` is integrated together with the node voltages, not after
them: within a time point each Newton round also takes a Newton step on
`w`'s backward-Euler equation, so the method matches what ngspice does
with the same model. (Operator splitting, voltages first, then `w`, is
allowed as an option for experiments, but the checks run the coupled
version.) Window functions and clamping of `w` follow the `.va` source
exactly.

## 8. Memristor models

### 8.1 Single source of truth

Each model is written once in Verilog-A (`models/<name>.va`), taken from
or matched to the model's paper.

- ngspice loads it compiled by OpenVAF (OSDI). Use the maintained fork
  (OpenVAF-Reloaded); confirm current status at setup.
- ent-lang uses a hand translation in `memristor.ent` at first, one
  `fn` per model, with the parameter order of the `.va` file.
- Check 3 is what proves the hand translation matches the `.va`.

### 8.2 Order

1. Biolek window HP model: simplest, the classic pinched loop.
2. VTEAM: threshold behaviour, the common crossbar choice.
3. Yakopcic: fitted to several real devices.

### 8.3 Xyce

Xyce is used for speed and for a cross-check on crossbars (§10, check 5).
Where Xyce has a built-in model matching ours, use it with identical
parameters; otherwise skip that model in Xyce. Verify at setup which
memristor levels the installed Xyce provides.

### 8.4 Later: generated models

A `va.py` that turns the supported subset of a `.va` model into
`memristor.ent` code, as `logic/words.py` writes Verilog out word by
word. Out of scope until the three models pass every check.

## 9. Circuits

### 9.1 Small, analytic

In `examples/analog/circuits/` (small enough to keep in the repo):
resistive divider, RC low-pass under step and sine, RC ladder of n
stages, a single memristor driven by a sine at three frequencies, a
memristor in series with a resistor.

### 9.2 Crossbars (the main experiment)

`crossbar.py N R_wire model variation seed` writes an N×N crossbar:
row drivers (grounded sources with `PULSE` or `SIN`), column sense
resistors to ground, a memristor at every crosspoint, and a wire
resistor between neighbouring crosspoints on every row and column
(`R_wire = 0` connects devices straight to the driver and sense lines,
which makes the circuit nearly decoupled). Initial states and parameter
variation are drawn from the seed by a deterministic rule written down
in both `crossbar.py` and the check scripts (the role of `drawn` in
`logic.ent`).

Sizes: 4, 16, 64, 256, 1024. Wire resistance: 0, 0.1, 1, 10 Ω per
segment. Two scenarios: read (small pulses, states barely move) and
write (large pulses on selected rows, states switch).

### 9.3 External stress tests (fetched, not in the repo)

- IBM power grid benchmarks (Nassif, 2008): large resistive/RC networks
  with published DC solutions. DC operating point only. Locate a current
  mirror in `fetch.sh`; if none is reachable, skip with a message.
- MCNC CircuitSim90/93: optional, only the circuits within §2's subset.

## 10. Checks

Each `check/*.py` names its number in its docstring, as in
`examples/logic/check/`. Each prints the first time point, node and
values where it fails.

1. **Parse** (`check/parse.py`): the model reads as many nodes, devices
   and probes as `circuit.net`'s header says, for every circuit in §9.
2. **Analytic** (`check/exact.py`): divider and RC circuits against
   closed-form solutions: DC exact to `1e-12` relative; transients
   within the integration method's expected error at the given step
   (halving the step must reduce the error by about 2× for backward
   Euler, 4× for trapezoidal).
3. **Against ngspice** (`check/spice.py`): every circuit in §9.1 and
   crossbars up to 64×64, against ngspice with the same `.va` models,
   run with tight tolerances (`reltol = 1e-6`, small `tmax`) so it acts
   as the converged reference. Compare on the model's time points
   (ngspice values interpolated linearly): pass when every probe is
   within `1e-3` of that probe's peak-to-peak range. For a single
   memristor under sine drive, also assert the pinched-loop properties:
   current is zero where voltage crosses zero, and the loop's area
   shrinks as frequency rises.
4. **Ways agree** (`check/agree.py`): ways 0, 1 and 2 give the same
   waveforms within the convergence tolerances of §7.1, on every
   circuit where the relaxation ways converge, and say which ones did
   not.
5. **Crossbars against Xyce** (`check/xyce.py`): crossbars of §9.2 up
   to 1024×1024 where Xyce finishes in reasonable time, same comparison
   as check 3.
6. **Same on any number of cores** (`check/same.py`): a sequential
   build and a parallel build on 1, 4, 16 and all threads give
   bit-identical `waves.txt`, every way.
7. **Power grids** (`check/grid.py`): DC operating point of the IBM
   grids against their published solutions, way 0 always; ways 1 and 2
   report whether they converged and in how many rounds.

## 11. Benchmarks

`bench/analog/run.py`, built like `bench/logic/run.py` and
`bench/run.py`:

- Before timing, runs the comparison of check 3 (or 5) and refuses to
  time a variant that disagrees.
- Competitors: ngspice with KLU (single core), Xyce serial, and Xyce
  parallel where it is built with MPI.
- Variants of the model: ways 0, 1, 2, sequential and parallel.
- Sweep: crossbar size × wire resistance × read/write scenario.
- Reports per run: wall time, time points, rejected steps, rounds per
  time point (mean and max), node updates per time point.
- Each measurement in its own process, round-robin over variants and
  sizes, conditions written into `bench/analog/RESULTS.md` (machine,
  pinning, OpenMP settings, versions of ngspice and Xyce).

The expected shape of the result, to confirm or refute: relaxation wins
clearly at low wire resistance and large N, the direct way wins at high
wire resistance, and the crossover moves with N. The interesting number
is where that crossover lies.

## 12. Milestones

Each ends with its checks passing and the README updated.

- **M0, read:** `prepare.py`, `circuit.ent`, check 1.
- **M1, DC:** devices without memory (R, sources), `.op` by way 0 with
  dense LU, then ways 1 and 2. Checks 2 (DC part) and 4.
- **M2, transient, fixed step:** capacitors, backward Euler, source
  waveforms. Check 2 (transient), check 3 on RC circuits, check 6.
- **M3, memristors:** Biolek, then VTEAM, then Yakopcic, coupled state
  per §7.3. Check 3 on the memristor circuits and crossbars up to 64×64.
- **M4, step control:** trapezoidal rule, LTE-based steps, breakpoints,
  rejection.
- **M5, scale:** KLU behind way 0, crossbars up to 1024×1024, checks 5
  and 7, benchmarks of §11.
- **M6, ensembles:** K variants of a crossbar side by side (device
  parameters drawn per variant from the seed), the analog counterpart of
  the logic simulator's 64 runs per word: Monte Carlo of device
  variation in one run. Each variant converges on its own; a time point
  is accepted when all have.
- **M7, later:** `view.ent`; generated models (§8.4); co-simulation with
  `examples/logic` (the PicoRV32 bus driving a crossbar as an
  accelerator, with the digital side's outputs becoming `PWL` source
  inputs).

## 13. Open questions and risks

- **Convergence of relaxation on crossbars with wire resistance.** The
  central risk and the central result. If Jacobi stalls, try
  Gauss–Seidel ordered by row (sequential within a row, rows in
  parallel), damping, or block relaxation (solve a whole row line
  directly, relax between lines).
- **Reading a field another entity sets in the same round.** The logic
  example's "Not here" names this for gates; Jacobi avoids it by reading
  the previous round's values. Confirm `old` columns cover it, or keep
  an explicit `v_round` field.
- **Stiffness.** Memristor switching is abrupt; step control (M4) must
  shrink the step through switching events, or ways 1 and 2 will need
  many rounds there.
- **Complex arithmetic** for AC analysis later: no complex type; would
  be real/imaginary field pairs and a complex solve in C.
- **Reference availability.** Confirm at setup: ngspice version with
  OSDI, OpenVAF build, Xyce version and its memristor models, a reachable
  source for the IBM grids. Write the versions into `RESULTS.md`.
- **Tolerances in check 3** (`1e-3` of range) are a starting point;
  tighten once the model is stable and record any change in this spec.
