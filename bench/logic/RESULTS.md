# The logic simulator: timings

`examples/logic` against Verilator 5.052, on the same gates (a circuit's
and-inverter graph, which Yosys 0.66 wrote both as the netlist the model
reads and as Verilog) and the same inputs.

## 2026-10-13: three ways, one core

AMD Ryzen AI 9 HX 370, Arch Linux, LLVM 22.1.8. `python3
bench/logic/run.py c6288 s38584 mem_ctrl hyp --check`: one core (CPU 2,
a Zen 5 core), native builds of both (`-O3 -march=native` for
Verilator's C++, `-O2 -march=native` for the model). A cycle's time is
what a run of three times as many cycles took more than a shorter one,
shared among the cycles it had more, so reading the circuit is not in
it; both work the inputs out by the same rule each cycle and fold every
output into a sum, and write nothing. One run each; the machine was
otherwise idle.

| circuit | ANDs | Verilator | as code | as rows | as entities |
|---|---|---|---|---|---|
| c6288 (a multiplier, 89 levels) | 1,870 | 1.31 us | 0.17 us | 2.89 us | 4.85 us |
| s38584 (1,274 flip-flops, 23 levels) | 8,594 | 3.09 us | 3.53 us | 15.63 us | 23.80 us |
| mem_ctrl | 40,863 | 14.47 us | 10.85 us | 69.66 us | 112.80 us |
| hyp (24,730 levels) | 238,658 | 115.82 us | 77.25 us | 393.53 us | 615.51 us |

The same as gates worked out a second, in millions. Verilator simulates
one run of the circuit; the model's values are 64 bits wide, 64 runs
side by side, which the last column counts:

| circuit | Verilator | as code | as rows | as entities | as code, 64 runs |
|---|---|---|---|---|---|
| c6288 | 1,423 | 11,006 | 647 | 386 | 704,386 |
| s38584 | 2,777 | 2,434 | 550 | 361 | 155,767 |
| mem_ctrl | 2,823 | 3,766 | 587 | 362 | 241,047 |
| hyp | 2,061 | 3,089 | 606 | 388 | 197,718 |

- **As entities** (`logic.ent`): every signal an entity, the ANDs gone
  through one after another by their level (`in turn by`), each looking
  its two inputs up. 2.6 ns an AND, the same for every circuit.
- **As rows** (`plain.ent`): no entities; the ANDs are rows of a buffer
  that a proc goes through. 1.6 ns an AND. The entities cost 1 ns an
  AND over this: the order worked out each cycle, and the look-ups.
- **As code** (`compile.py`, `coded.ent`): the netlist written out as
  ent-lang, an AND a line, and compiled with the program. This is what
  Verilator does, and it is the like-for-like comparison: 7.7 times
  faster than Verilator on c6288, 1.3 and 1.5 times on mem_ctrl and hyp,
  and 0.88 of its speed on s38584.
- Going through a table of gates is 4 to 8 times slower than Verilator
  for one run, and cannot be otherwise: each AND's value goes through
  memory to the next. With the 64 runs counted, the table as entities
  works out 8 to 17 times the gates Verilator does, and the code 50 to
  490 times.

How long the builds take, for the largest (hyp, 238,658 ANDs): the model
as code 111 s (written as 120 systems of 2,000 ANDs each), Verilator 262
s on all the cores. The table-driven forms are built once, in a second,
for any circuit.

### How the code got there (s38584, a cycle)

| | a cycle |
|---|---|
| ANDs as `let`s in procs, the rest read from and written to a buffer | 8.3 us |
| every signal a field of one unique | 5.7 us |
| ... but an AND that only its own system reads a `let` | 3.5 us |

A value read from a buffer is checked against the buffer's length each
time, and of two thousand `let`s in a proc most live on the stack. A
field of a unique is at a place the compiler knows, as a member of
Verilator's struct is; and what is read only nearby need not be kept at
all (1,977 of the 8,594 ANDs are fields).

## 2026-10-13, later: a processor, and only the gates whose inputs changed

PicoRV32 (`examples/logic/cpu.v`, 30,383 ANDs and 1,943 flip-flops)
running Dhrystone to its end, 583,031 clock cycles, with its memory
outside the circuit; one core (CPU 2), a native build, the whole run
timed with its start (which is a few hundredths of a second). All ways
give the same outputs in every cycle, which are Icarus Verilog's
(`check/cpu.py`).

| the gates | the run | a cycle | cycles a second |
|---|---|---|---|
| as code | 3.0 s | 5.1 us | 196,000 |
| as entities, those whose inputs changed | 19.8 s | 33.9 us | 29,500 |
| as rows | 28.3 s | 48.5 us | 20,600 |
| as entities, all | 46.8 s | 80.3 us | 12,500 |

(As code and as rows were timed with the ordinary build of the check,
not a native one, and the second line while writing the outputs to a
file, 42 MB: each is a little worse than it would be.)

- Going through only the ANDs whose inputs changed (`events.ent`), an
  AND works itself out 1,935 times a cycle on average, of 30,383 ANDs:
  one in sixteen, in 17 rounds a cycle. That is 2.4 times faster than
  going through all of them as entities, and faster than the rows.
  What it costs is 17 ns for each time an AND works itself out, where
  the pass through all takes 2.6 ns an AND: the log of what changed, the
  wires as edges, and what is sent along them.
- It pays only where little changes. With inputs drawn anew every
  cycle, and 64 runs in each value, nearly every AND changes every
  cycle and settles more than once: s38584 takes 239 us a cycle this way
  (17,950 times an AND works itself out, twice as many as there are)
  where the pass through all takes 26.
- Before the compiler followed the log for a `for` that sends whole
  numbers (it went through every entity each round), the processor's
  cycle took 363 us this way.
- Verilator has not been run on the processor yet: it needs the same
  memory around it in C++.
