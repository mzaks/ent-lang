# A gate-level logic simulator in ent-lang

Runs a digital circuit, given as a netlist of gates and flip-flops,
clock cycle by clock cycle, and gives the bits that
[Icarus Verilog](https://github.com/steveicarus/iverilog) gives for the
same gates and inputs; its speed is held against
[Verilator](https://www.veripool.org/verilator/). It is being built in
steps; what is here reads a circuit, runs it two ways, and is checked on
49 circuits.

- `circuit.ent`: the circuit from its file, an and-inverter graph in the
  ASCII AIGER form (two-input ANDs, inverters on the wires, flip-flops on
  one clock). Every signal becomes an entity; an AND has its two inputs
  as entities and a level, one more than the higher of its inputs'.
- `logic.ent`: a clock cycle. The inputs get their values, the ANDs are
  gone through one after another by their level (`in turn by`), the
  outputs are noted, the flip-flops take what stands at their inputs. A
  value is 64 bits: 64 runs side by side, each with inputs of its own,
  which are worked out from the seed, the input and the cycle.
- `plain.ent`: the same circuit without entities, as rows of buffers
  gone through by a proc: what the entities cost is the difference.
- `simulate.ent`: the program. It reads "circuit.aag" and "run.txt"
  (cycles, seed, whether to write outputs, which way) where it is
  started, and writes "outputs.txt", a line of 0s and 1s a cycle.

```sh
examples/logic/fetch.sh             # once: the circuits, into build/
examples/logic/prepare.py c6288     # Yosys: the netlist, and its Verilog
cd build/logic/run && cp ../circuits/c6288.aag circuit.aag
../../../tools/ent run ../../../examples/logic/simulate.ent
```

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
- `bench/logic/run.py --check` holds Verilator's outputs against the
  model's before it times either.

## Still to come

Going through the gates level by level on all the cores, and only
through those whose inputs changed; a processor; the timings written up.
