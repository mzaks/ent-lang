# A gate-level logic simulator in ent-lang

Runs a digital circuit, given as a netlist of gates and flip-flops,
clock cycle by clock cycle, and gives the bits that
[Icarus Verilog](https://github.com/steveicarus/iverilog) gives for the
same gates and inputs; its speed is held against
[Verilator](https://www.veripool.org/verilator/). It is being built in
steps; what is here reads a circuit, runs it four ways, is checked on
49 circuits, and runs a processor through a program. Written out as code
and compiled, a circuit runs as fast as under Verilator or faster on
three of four, and the processor's gates 1.8 times as fast; with the
processor's Verilog written out word by word the model is at 0.8 of
Verilator's speed on that Verilog (`bench/logic/RESULTS.md`). `view.ent`
shows a small circuit at work in a window.

- `circuit.ent`: the circuit from its file, an and-inverter graph in the
  ASCII AIGER form (two-input ANDs, inverters on the wires, flip-flops on
  one clock). Every signal becomes an entity; an AND has its two inputs
  as entities and a level, one more than the higher of its inputs'.
- `logic.ent`: a clock cycle. The inputs get their values, the ANDs are
  gone through one after another by their level (`in turn by`), the
  outputs are noted, the flip-flops take what stands at their inputs. A
  value is 64 bits: 64 runs side by side, each with inputs of its own,
  which are worked out from the seed, the input and the cycle.
- `events.ent`: going through only the ANDs whose inputs changed. A
  signal that got another value tells the ANDs that read it, along the
  wires (edges of `Feeds`); an AND that was told works itself out again
  and, where it comes to another value, tells its own readers: reactive
  `for`s, in rounds until nothing is told.
- `plain.ent`: the same circuit without entities, as rows of buffers
  gone through by a proc: what the entities cost is the difference.
- `compile.py` and `coded.ent`: the circuit written out as ent-lang
  code, every AND a line (a field of one unique where it is read far
  off, else a `let`), and compiled with the program: what Verilator does
  with a circuit.
- `words.py` and `words.ent`: the circuit written out word by word
  from its Verilog: Yosys leaves an addition an addition, a choice
  between two words one choice and a memory a memory, and each is a
  line on a u64 (a memory a buffer). What Verilator does with Verilog.
  It is the module `coded.ent` runs, in place of the gates; a value is
  one run then, not 64. `ops.v` is a circuit of every operation of
  Verilog, for checking it.
- `cpu_words.ent`: the processor word by word, its memory answering in
  whole words: the fastest way here, 11 million cycles a second.
- `view.ent`: a small circuit in a window, its gates column by column
  by their level and its wires lit where they carry a 1, a level a
  step: Space holds it, the right arrow steps, up and down change the
  pace.
- `bus.ent`, `cpu.ent`, `cpu_coded.ent`: a processor running a program.
  The circuit is PicoRV32 (`cpu.v`: 30,383 ANDs, 1,943 flip-flops) and
  its memory is outside the circuit: a buffer of words that answers the
  processor's bus, a cycle after it is asked. The program is PicoRV32's
  Dhrystone, built with clang for RISC-V.
- `simulate.ent`: the program for the first two ways. It reads "circuit.aag" and "run.txt"
  (cycles, seed, whether to write outputs, which way) where it is
  started, and writes "outputs.txt", a line of 0s and 1s a cycle.

```sh
examples/logic/fetch.sh             # once: the circuits, into build/
examples/logic/prepare.py c6288     # Yosys: the netlist, and its Verilog
cd build/logic/run && cp ../circuits/c6288.aag circuit.aag
../../../tools/ent run ../../../examples/logic/simulate.ent

cp ../circuits/s27.aag circuit.aag     # a small one, in a window
../../../tools/ent run ../../../examples/logic/view.ent

examples/logic/prepare.py cpu && examples/logic/words.py cpu
../../../tools/ent run -I ../words/cpu ../../../examples/logic/cpu_words.ent
```
(The last needs "memory.bin", the program, where it is started:
`check/cpu.py` builds Dhrystone and leaves one in `build/logic/cpu/coded`.)

The circuits are ISCAS-85, ISCAS-89 and the EPFL combinational suite,
which `fetch.sh` puts under `build/` (they are not in this repository).
`prepare.py` has Yosys turn a circuit's Verilog into the netlist and
into the same gates as Verilog again, so the model, Icarus and Verilator
all simulate the identical gates. Needs yosys, iverilog and verilator.

## What is checked

- `check/parse.py`: the model has as many inputs, flip-flops, outputs
  and ANDs as each file's first line says, and every AND after both its
  inputs (49 circuits, from 2 ANDs to 238,658, up to 24,730 levels).
- `check/equal.py`: every output of every cycle is Icarus Verilog's,
  for the first of the 64 runs, whose inputs a testbench made by the
  script gives to Icarus: over 10,000 cycles for 43 circuits, and over
  200 for the six deep arithmetic ones, where Icarus takes minutes.
- `check/agree.py`: the four ways give the same outputs, and the same
  number made of all outputs of all 64 runs, on every circuit.
- `check/cpu.py`: the processor runs Dhrystone to its end, 583,031
  clock cycles, with the same outputs in every cycle the four ways,
  word by word, and under Icarus Verilog (which is given a memory that answers alike), and
  the program says the same, with every value it checks as it should
  be. As code that takes 3 seconds (196,000 cycles a second), as rows
  28, as entities 47, and 20 going through only the ANDs whose inputs
  changed: one in sixteen a cycle.
- `check/words.py`: a circuit written out word by word gives Icarus
  Verilog's outputs for its gates: all 49 circuits over 200 cycles
  (hyp over 8), and `ops` over 10,000 (every operation, words of many widths, flip-flops with
  first values, a memory written a byte at a time).
- `check/same.py`: a build for one core and a build for all of them, on
  1, 4, 16 and all threads, give the same outputs every way.
- `bench/logic/cpu.py` holds Verilator's outputs for the processor
  against the model's, cycle by cycle, for the gates and for the
  processor's Verilog as written, before it times them: on the same
  gates the model as code is 1.8 times as fast as Verilator; given the
  Verilog as written, Verilator takes 0.071 us a cycle and the model,
  word by word, 0.087.
- `bench/logic/run.py --check` holds Verilator's outputs against the
  model's before it times either.

## Not here

- Going through the ANDs of a level on several cores: a `for` would have
  to read, of another entity, a field it sets itself, where the order
  makes that safe.
- Word by word: signals wider than 64 bits, memories read at the
  clock's edge, and what Verilog leaves open (a division by 0 gives 0;
  bits picked from outside a word are 0).
- Values a wire can have besides 0 and 1, delays, and more than one
  clock.
