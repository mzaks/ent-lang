# An analog circuit simulator in ent-lang

Simulates circuits of resistors, capacitors, sources and memristors in
time, node voltage by node voltage, to give the waveforms that
[ngspice](https://ngspice.sourceforge.io/) gives for the same circuit;
then to be faster than ngspice and Xyce on large memristor crossbars by
relaxation, every node solving for its own voltage. `analog-SPEC.md`
says what it is to be and in which milestones. It is being built in
steps; what is here is milestone M0: a circuit is read.

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
  with a resistor, set and reset by pulses.
- `models/biolek.va`: the HP memristor with Biolek's window in
  Verilog-A, which says the order of its parameters. It is first held
  against anything in milestone M3.

```sh
examples/analog/prepare.py all            # the circuits, into build/analog/circuits
examples/analog/crossbar.py 64 1 biolek 0.1 1 write
examples/analog/prepare.py crossbar_64_1_biolek_write
examples/analog/check/parse.py
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
- ngspice runs the decks `prepare.py` writes for the circuits without
  memristors (not yet a numbered check: that is check 3, from M2).

## Not here yet

- Solving anything: DC (M1), transients (M2), memristors (M3).
- The deck for Xyce, and `fetch.sh` for the IBM power grids: with M5,
  when Xyce is installed.
- Memristors in ngspice's deck are written as OpenVAF devices, untried
  until OpenVAF is there.
- A capacitor's `IC=` is kept, and how it is used (ngspice uses it only
  with `.tran ... uic`) is settled in M2.
- `v(a,b)` probes; a probe is a node's voltage or a device's current.

## Where it goes beyond the spec

- `circuit.net` has a line `op` where the netlist asks for the
  operating point (section 4 has only `tran`).
- A memristor is written `X<name> a b <model> name=value ...` in the
  SPICE netlist; the parameters not given take the `.va` file's
  defaults.

## Found in the language on the way

- A float is written into text with six digits after the point, so
  1e-14 comes out as 0.0: `waves.txt` (M2) needs its numbers written
  in full, which wants a formatter.
- A `for` over edges cannot add into a unique (`Seen.edges += 1`), nor
  into a `var` from outside it; the check counts edges by sending to the
  node at the other end instead.
- A line that ends in a name, followed by one that starts with a
  bracket, is read as a call: `let s = a + b` and then `(s, r)` on the
  next line is `b(s, r)`. `circuit.ent` puts such sums in brackets.
