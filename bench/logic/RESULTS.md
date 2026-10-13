# The logic simulator: timings

`examples/logic` against Verilator 5.052, on the same gates (a circuit's
and-inverter graph, which Yosys 0.66 wrote both as the netlist the model
reads and as Verilog) and the same inputs.

## 2026-10-13: four circuits, every way

AMD Ryzen AI 9 HX 370 (4 Zen 5 cores, 8 Zen 5c, 24 threads), Arch
Linux, LLVM 22.1.8. `python3 bench/logic/run.py --check`: one core (CPU
2, a Zen 5 core), native builds of all (`-O3 -march=native` for
Verilator's C++, `-O2 -march=native` for the model). A cycle's time is
what a run of three times as many cycles took more than a shorter one,
shared among the cycles it had more, so reading the circuit is not in
it; all work the inputs out by the same rule each cycle and fold every
output into a sum, and write nothing. The machine was otherwise idle;
one run each, but for Verilator built for four threads (on the four
Zen 5 cores, the median of three). Before the timing, Verilator's
outputs and the code's were the model's for 1,000 cycles; all of the
model's are Icarus Verilog's (`examples/logic/check/equal.py`).

A clock cycle:

| circuit | ANDs | Verilator | Verilator, 4 threads | as code | as rows | as entities | by what changed | Icarus Verilog |
|---|---|---|---|---|---|---|---|---|
| c6288 (a multiplier, 89 levels) | 1,870 | 1.34 us | 1.32 us | 0.22 us | 2.90 us | 4.95 us | 403 us | 1,085 us |
| s38584 (1,274 flip-flops, 23 levels) | 8,594 | 3.05 us | 4.65 us | 3.70 us | 15.98 us | 25.07 us | 239 us | 635 us |
| mem_ctrl | 40,863 | 14.16 us | 14.08 us | 13.50 us | 70.78 us | 114.27 us | 3,351 us | 4,947 us |
| hyp (24,730 levels) | 238,658 | 114.29 us | 119.96 us | 79.21 us | 389.82 us | 609.22 us | 1.2 s | 9.5 s |

The same as gates worked out a second, in millions (Verilator and
Icarus simulate one run of the circuit; the model's values are 64 bits
wide, 64 runs side by side, which the sixth column counts), how long
Verilator and the code take to build, and the most memory each has:

| circuit | Verilator | as code | as rows | as entities | as code, 64 runs | built: Verilator | as code | memory: Verilator | as code | the tables |
|---|---|---|---|---|---|---|---|---|---|---|
| c6288 | 1,398 | 8,319 | 645 | 377 | 532,392 | 5 s | 1 s | 4 MB | 4 MB | 92 MB |
| s38584 | 2,818 | 2,320 | 538 | 343 | 148,480 | 8 s | 4 s | 4 MB | 4 MB | 116 MB |
| mem_ctrl | 2,885 | 3,027 | 577 | 358 | 193,757 | 24 s | 11 s | 5 MB | 4 MB | 98 MB |
| hyp | 2,088 | 3,013 | 612 | 392 | 192,821 | 255 s | 102 s | 7 MB | 6 MB | 118 MB |

- **As code** (`compile.py`, `coded.ent`): the netlist written out as
  ent-lang, an AND a line, and compiled with the program. This is what
  Verilator does, and the like-for-like comparison: 6 times as fast as
  Verilator on c6288, 1.4 times on hyp, the same on mem_ctrl, and 0.8 of
  its speed on s38584. It builds in less than half of Verilator's time.
- **As rows** (`plain.ent`): no entities; the ANDs are rows of a buffer
  that a proc goes through. 1.6 to 1.9 ns an AND, 2 to 5 times
  Verilator's time: each AND's value goes through memory to the next.
- **As entities** (`logic.ent`): every signal an entity, the ANDs gone
  through one after another by their level (`in turn by`), each looking
  its two inputs up. 2.6 to 2.9 ns an AND: the entities cost 1 ns an AND
  over the rows (the order worked out each cycle, and the look-ups).
- **By what changed** (`events.ent`): with inputs drawn anew each cycle
  nearly every AND changes, and settles more than once, round after
  round: 10 to 2,000 times the time of going through all of them, most
  where the circuit is deep (hyp has a round for each of its 24,730
  levels). It is for a circuit in which little changes: see the
  processor, below.
- **Verilator built for four threads** is no faster on any of these
  than on one, and slower on s38584: gates this small are not worth
  handing to another thread.
- **Icarus Verilog**, which goes by events too, takes 200 to 80,000
  times Verilator's time; it is what the outputs are held against, not
  what the speed is.
- Counting the 64 runs, the code works out 50 to 380 times the gates a
  second that Verilator does, for whoever needs many runs at once.
- The tables' memory is the room the program has for a circuit of a
  million signals, not this circuit's: a program is built once for any
  circuit. The code is built for one, and has what that one needs.
- The model's ways run on one thread each. One core and all the cores
  give the same outputs (`check/same.py`), and no faster: nothing here
  is shared out among threads. Going through a level's ANDs on several
  would need a `for` to read, of another entity, a field that it sets
  itself where the order makes that safe, which the language does not
  have; and the deep circuits have ten ANDs a level.
- The circuits of five to eight million six-input LUTs that the EPFL
  suite has besides were not run: turning one into ANDs and compiling it
  takes hours.

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

## 2026-10-13, later: a processor, against Verilator

PicoRV32 (`examples/logic/cpu.v`, 30,383 ANDs and 1,943 flip-flops)
running Dhrystone to its end, 583,031 clock cycles, with its memory
outside the circuit. `python3 bench/logic/cpu.py`: one core (CPU 2),
native builds, nothing written but a sum of the outputs, the whole run
with its start, the least of three. Verilator has the same memory
around the processor, in C++, and runs twice: the gates Yosys made of
the processor, which are what the model runs, and the processor's
Verilog as it was written. Before the timing, every output of every
cycle and what the program says were the same in both of Verilator's
runs and the model's (and they are Icarus Verilog's: `check/cpu.py`).

| | the run | a cycle | cycles a second | built in |
|---|---|---|---|---|
| Verilator, the Verilog as written | 0.17 s | 0.29 us | 3,428,000 | 5 s |
| the model, the gates as code | 2.63 s | 4.52 us | 221,000 | 8 s |
| Verilator, the gates | 4.65 s | 7.98 us | 125,000 | 14 s |
| the model, as entities, those whose inputs changed | 19.76 s | 33.90 us | 29,500 | |
| the model, as rows | 28.63 s | 49.11 us | 20,400 | |
| the model, as entities, all | 46.53 s | 79.81 us | 12,500 | |

- On the same gates the model as code is 1.8 times as fast as
  Verilator, and builds in less time.
- Given the Verilog as it was written, Verilator is 16 times as fast as
  that again: it adds two 32-bit numbers with one instruction where the
  gates take a few hundred ANDs, and it leaves out what a cycle does not
  need. That is the number for somebody who has the Verilog; the gates
  are what is left when all one has is a netlist, and what this model
  is about.
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

## 2026-10-13, last: the processor word by word

The same processor and program, with a way more: the processor's
Verilog written out as ent-lang word by word (`examples/logic/words.py`:
Yosys leaves an addition an addition and a memory a memory, 611
operations, 131 flip-flop words and the 32 registers as a buffer, where
the gates are 30,383 ANDs and 1,943 flip-flops). `python3
bench/logic/cpu.py` again, the machine idle. Every output of every
cycle is the gates' (and so Icarus Verilog's) before anything is timed.

| | the run | a cycle | cycles a second | built in |
|---|---|---|---|---|
| Verilator, the Verilog as written, driven in words | 0.041 s | 0.071 us | 14,080,000 | 5 s |
| the model, word by word, driven in words | 0.051 s | 0.087 us | 11,520,000 | 1 s |
| Verilator, the Verilog as written | 0.16 s | 0.27 us | 3,681,000 | 5 s |
| the model, word by word | 0.22 s | 0.38 us | 2,633,000 | 1 s |
| the model, the gates as code | 2.49 s | 4.28 us | 234,000 | 8 s |
| Verilator, the gates | 4.75 s | 8.15 us | 123,000 | 17 s |
| the model, as entities, those whose inputs changed | 19.77 s | 33.90 us | 29,500 | |
| the model, as rows | 28.66 s | 49.16 us | 20,300 | |
| the model, as entities, all | 46.55 s | 79.84 us | 12,500 | |

(The first two: the least of fifteen runs, less the 1 ms a run of a
thousand cycles takes; the rest the least of three, with their start.)

- **Driven in words** is how a testbench drives Verilator: the memory
  around the processor gives it a whole word and reads a whole address
  (`cpu_words.ent`, and the harness's like), and one sum a cycle is made
  of the address and the word; both sums are the same. Word by word the
  model takes 0.087 us a cycle and Verilator 0.071: Verilator is 1.2
  times as fast, where it was 16 times as fast as the model's gates.
  The model builds in a second.
- The two rows under them carry every input and output bit by bit
  through `Stim` and `Seen` and fold each of the 71 output bits into the
  sum, as all the rows below do. Three quarters of the 0.27 us that was
  written down for Verilator on the Verilog as written, above, is that
  sum and not the processor; so is most of the model's 0.38 us.
- From the gates to the words the model's cycle is 49 times shorter
  (4.28 us to 0.087): it was the gates that cost, not the language.
- An `-O3` build of the model is no faster than its `-O2`.
- A value is one run here, not 64 side by side.
- Writing a circuit out word by word says nothing new about a circuit
  that is gates to begin with: the ISCAS and EPFL circuits come out as
  one operation a gate or more (hyp: 420,512 operations for 238,658
  ANDs). They were run for the check only (`check/words.py`: all 49
  the same as Icarus Verilog, and `ops`, a circuit of every operation
  of Verilog).
- On the way the compiler's reading of a long line was mended: it asked
  for the type of each side of an operator three times, and each asking
  asked the same of what is inside, so a line 32 operators deep (the
  bits of a word put together) did not end.
