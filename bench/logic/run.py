#!/usr/bin/env python3
"""Times the logic simulator (examples/logic) against Verilator.

  examples/logic/fetch.sh
  examples/logic/prepare.py all
  python3 bench/logic/run.py [circuits] [--check]

For each circuit (c6288 s38584 hyp mem_ctrl by default) both simulate
the same gates with the same inputs, worked out by the same rule from
the seed, the input and the cycle, and write nothing: each keeps a sum
of its outputs. Each runs twice, for so many cycles and for three times
as many, and a cycle's time is what the longer run took more, shared
among the cycles it had more: reading the circuit and starting up are
not in it. One core, CPU 2 (a fast one), native builds of both.

The model is timed both ways it can go through the gates: as entities
by their level, and as rows of buffers (examples/logic/plain.ent).
Verilator simulates one run of the circuit, the model 64 side by side:
said are a cycle's time for each, and how many gates a second that is
(for the model with its 64 runs counted, too).

With --check Verilator's outputs are first held against the model's own
first run, cycle by cycle, for 1,000 cycles.
"""

import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
BENCH = os.path.join(ROOT, "build", "logic", "bench")
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "examples", "logic", "check"))
import equal  # the netlist's names, and how the Verilog has them

HARNESS = r"""
#include "Vwrap.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static uint64_t mix(uint64_t x) {
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}
static uint64_t drawn(uint64_t seed, int index, long cycle) {
  return mix(mix(seed + 0x9e3779b97f4a7c15ull * (uint64_t)(index + 1)) ^
             ((uint64_t)cycle * 0xd1342543de82ef95ull + 1));
}
template <class T> static void put(T &all, int at, bool bit) {
  all = (T)((all & ~((T)1 << at)) | ((T)bit << at));
}
template <std::size_t N> static void put(VlWide<N> &all, int at, bool bit) {
  all[at / 32] = (all[at / 32] & ~(1u << (at % 32))) | ((uint32_t)bit << (at % 32));
}
template <class T> static bool get(const T &all, int at) { return (all >> at) & 1; }
template <std::size_t N> static bool get(const VlWide<N> &all, int at) {
  return (all[at / 32] >> (at % 32)) & 1;
}

int main(int argc, char **argv) {
  long cycles = atol(argv[1]);
  bool shown = argc > 2;
  Vwrap *top = new Vwrap;
  uint64_t sum = 0;
  top->clk = 0;
  for (long cycle = 0; cycle < cycles; ++cycle) {
    for (int i = 0; i < INPUTS; ++i)
      put(top->in, i, drawn(1, i, cycle) & 1);
    top->eval();
    for (int o = 0; o < OUTPUTS; ++o) {
      bool bit = get(top->out, o);
      sum = mix(sum ^ bit);
      if (shown)
        putchar('0' + bit);
    }
    if (shown)
      putchar('\n');
    top->clk = 1;
    top->eval();
    top->clk = 0;
  }
  if (!shown)
    printf("sum %llu\n", (unsigned long long)sum);
  delete top;
  return 0;
}
"""


def verilator(name):
    """Verilator's program for a circuit: its gates, with the inputs and
    the outputs each as one port of so many bits."""
    inputs, outputs = equal.netlist(name)
    declared, clock = equal.ports(name)
    folder = os.path.join(BENCH, "verilator_" + name)
    built = os.path.join(folder, "obj_dir", "Vwrap")
    os.makedirs(folder, exist_ok=True)
    quoted = lambda p: p if re.fullmatch(r"\w+", p) else "\\" + p + " "
    widths = {}
    for symbol in inputs + outputs:
        part = re.fullmatch(r"(.+)\[(\d+)\]", symbol)
        if part and part.group(1) in declared and declared[part.group(1)]:
            widths[part.group(1)] = max(widths.get(part.group(1), 0), int(part.group(2)) + 1)
        else:
            widths.setdefault(symbol, 1)
    nets = "\n".join(f"  wire {'[%d:0] ' % (wide - 1) if declared.get(port) else ''}"
                     f"{quoted(port)};" for port, wide in widths.items())
    drives = "\n".join(f"  assign {equal.wire(symbol, declared)} = in[{index}];"
                       for index, symbol in enumerate(inputs))
    shows = "\n".join(f"  assign out[{index}] = {equal.wire(symbol, declared)};"
                      for index, symbol in enumerate(outputs))
    joins = ", ".join(f".{quoted(p)}({quoted(p)})" for p in declared if p != clock)
    if clock:
        joins += (", " if joins else "") + f".{quoted(clock)}(clk)"
    with open(os.path.join(folder, "wrap.v"), "w") as file:
        file.write(f"""module wrap(input clk, input [{max(1, len(inputs)) - 1}:0] in,
            output [{max(1, len(outputs)) - 1}:0] out);
{nets}
{drives}
{shows}
  {quoted(name)} dut({joins});
endmodule
""")
    with open(os.path.join(folder, "harness.cpp"), "w") as file:
        file.write(f"#define INPUTS {len(inputs)}\n#define OUTPUTS {len(outputs)}\n" + HARNESS)
    subprocess.run(
        ["verilator", "--cc", "--exe", "--build", "-j", "0", "-O3", "--x-assign", "fast",
         "--x-initial", "fast", "--noassert", "-Wno-fatal", "--top-module", "wrap",
         "-CFLAGS", "-O3 -march=native", "wrap.v", os.path.join(CIRCUITS, name + ".v"),
         "harness.cpp"],
        check=True, cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return built


def model():
    built = os.path.join(BENCH, "simulate")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    environment.setdefault("ENT_CFLAGS", "-march=native")
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
         "-I", os.path.join(ROOT, "examples", "logic"),
         os.path.join(ROOT, "examples", "logic", "simulate.ent"), "-o", built],
        check=True, env=environment)
    return built


def seconds(command, folder):
    started = time.time()
    subprocess.run(["taskset", "-c", "2"] + command, check=True, cwd=folder,
                   stdout=subprocess.DEVNULL)
    return time.time() - started


def a_cycle(run):
    """A cycle's seconds: `run(cycles)` times a run of so many. The number
    of cycles is raised until the shorter run takes a quarter of a second."""
    cycles = 100
    while True:
        short = run(cycles)
        if short >= 0.25 or cycles >= 100000000:
            break
        cycles *= 4
    return (run(3 * cycles) - short) / (2 * cycles)


def main():
    arguments = sys.argv[1:]
    names = [a for a in arguments if not a.startswith("--")] or \
        ["c6288", "s38584", "hyp", "mem_ctrl"]
    os.makedirs(BENCH, exist_ok=True)
    simulate = model()
    print("| circuit | ANDs | Verilator, a cycle | as entities | as rows | "
          "gates a second: Verilator | as entities | as rows | "
          "with 64 runs: as entities | as rows |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for name in names:
        folder = os.path.join(BENCH, "model_" + name)
        os.makedirs(folder, exist_ok=True)
        shutil.copy(os.path.join(CIRCUITS, name + ".aag"), os.path.join(folder, "circuit.aag"))
        with open(os.path.join(folder, "circuit.aag")) as file:
            gates = int(file.readline().split()[5])
        reference = verilator(name)

        def ours(cycles, shown=0, way=0):
            with open(os.path.join(folder, "run.txt"), "w") as file:
                file.write(f"{cycles} 1 {shown} {way}\n")
            return seconds([simulate], folder)

        if "--check" in arguments:
            ours(1000, 1)
            wanted = open(os.path.join(folder, "outputs.txt")).read()
            got = subprocess.run([reference, "1000", "show"], check=True,
                                 capture_output=True, text=True).stdout
            if wanted != got:
                sys.exit(f"{name}: Verilator's outputs are not the model's")
        theirs = a_cycle(lambda cycles: seconds([reference, str(cycles)], folder))
        mine = a_cycle(ours)
        plain = a_cycle(lambda cycles: ours(cycles, 0, 1))
        print(f"| {name} | {gates} | {1e6 * theirs:.2f} us | {1e6 * mine:.2f} us | "
              f"{1e6 * plain:.2f} us | {gates / theirs / 1e6:.0f} M | "
              f"{gates / mine / 1e6:.0f} M | {gates / plain / 1e6:.0f} M | "
              f"{64 * gates / mine / 1e6:.0f} M | {64 * gates / plain / 1e6:.0f} M |",
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
