#!/usr/bin/env python3
"""Times the logic simulator (examples/logic) against Verilator.

  examples/logic/fetch.sh
  examples/logic/prepare.py all
  python3 bench/logic/run.py [circuits] [--check]

For each circuit (c6288 s38584 mem_ctrl hyp by default) every simulator
runs the same gates with the same inputs, worked out by the same rule
from the seed, the input and the cycle, and writes nothing: each keeps a
sum of its outputs. Each runs twice, for so many cycles and for three
times as many, and a cycle's time is what the longer run took more,
shared among the cycles it had more: reading the circuit and starting up
are not in it. One core, CPU 2 (a fast one), native builds.

The model is timed four ways: the gates as entities gone through by
their level, as entities of which only those are gone through whose
inputs changed (examples/logic/events.ent), as rows of buffers
(plain.ent), and written out as ent-lang code that is compiled with the
program (compile.py, coded.ent), which is what Verilator does with a
circuit. Verilator is timed on one thread and built for four (on the
four fast cores, the median of three); Icarus Verilog, which the
outputs are held against (check/equal.py), is timed for the scale of it.
Verilator and Icarus simulate one run of the circuit, the model 64 side
by side: said are a cycle's time for each, how many gates a second that
is (for the code with its 64 runs counted, too), how long Verilator and
the code took to build, and the most memory each had.

With --check Verilator's outputs and the code's are first held against
the model's own first run, cycle by cycle, for 1,000 cycles.
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


def verilator(name, threads=0):
    """Verilator's program for a circuit: its gates, with the inputs and
    the outputs each as one port of so many bits. (With `threads`, built
    to run on so many.)"""
    inputs, outputs = equal.netlist(name)
    declared, clock = equal.ports(name)
    folder = os.path.join(BENCH, f"verilator_{name}" + (f"_{threads}" if threads else ""))
    built = os.path.join(folder, "obj_dir", "Vwrap")
    # (Built anew each time, so that how long it takes can be said.)
    shutil.rmtree(os.path.join(folder, "obj_dir"), ignore_errors=True)
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
         *(["--threads", str(threads)] if threads else []),
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


def code(name):
    """The circuit written out as ent-lang code and built
    (examples/logic/compile.py, coded.ent); and how long the build took."""
    subprocess.run([sys.executable, os.path.join(ROOT, "examples", "logic", "compile.py"),
                    name], check=True, stdout=subprocess.DEVNULL)
    built = os.path.join(BENCH, "coded_" + name)
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    environment.setdefault("ENT_CFLAGS", "-march=native")
    # (Not taken from the cache of builds: the time is the build's.)
    environment["ENT_CACHE"] = os.path.join(BENCH, "no_cache_" + name)
    shutil.rmtree(environment["ENT_CACHE"], ignore_errors=True)
    started = time.time()
    subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
         "-I", os.path.join(ROOT, "examples", "logic"),
         "-I", os.path.join(ROOT, "build", "logic", "code", name),
         os.path.join(ROOT, "examples", "logic", "coded.ent"), "-o", built],
        check=True, env=environment)
    return built, time.time() - started


PEAK = {}


def seconds(command, folder, cpus="2", what=None):
    """How long a run took; the most memory it had is noted under `what`.
    (Read from the process while it runs: what the system says at its end
    is no less than this script's own memory, which every process it
    starts begins with.)"""
    started = time.time()
    process = subprocess.Popen(["taskset", "-c", cpus] + command, cwd=folder,
                               stdout=subprocess.DEVNULL)
    most = 0
    while process.poll() is None:
        if what:
            try:
                with open(f"/proc/{process.pid}/status") as status:
                    for line in status:
                        if line.startswith("VmHWM:"):
                            most = max(most, int(line.split()[1]) / 1024)
            except OSError:
                pass
        time.sleep(0.002)
    if process.returncode:
        sys.exit(f"{' '.join(command)} failed")
    if what:
        PEAK[what] = max(PEAK.get(what, 0), most)
    return time.time() - started


def a_cycle(run, first=100):
    """A cycle's seconds: `run(cycles)` times a run of so many. The number
    of cycles is raised until the shorter run takes a quarter of a second."""
    cycles = first
    while True:
        short = run(cycles)
        if short >= 0.25 or cycles >= 100000000:
            break
        cycles *= 4
    return (run(3 * cycles) - short) / (2 * cycles)


def icarus(name):
    """A cycle's seconds under Icarus Verilog, by check/equal.py's
    testbench: two runs, as above (it reads the circuit anew each time,
    which the difference leaves out)."""
    def run(cycles):
        started = time.time()
        equal.reference(name, cycles)
        return time.time() - started
    return a_cycle(run, 20)


def main():
    arguments = sys.argv[1:]
    names = [a for a in arguments if not a.startswith("--")] or \
        ["c6288", "s38584", "mem_ctrl", "hyp"]
    os.makedirs(BENCH, exist_ok=True)
    simulate = model()
    rows = []
    for name in names:
        folder = os.path.join(BENCH, "model_" + name)
        os.makedirs(folder, exist_ok=True)
        shutil.copy(os.path.join(CIRCUITS, name + ".aag"), os.path.join(folder, "circuit.aag"))
        with open(os.path.join(folder, "circuit.aag")) as file:
            gates = int(file.readline().split()[5])
        started = time.time()
        reference = verilator(name)
        their_build = time.time() - started
        threaded = verilator(name, 4)
        fast, my_build = code(name)

        def ours(cycles, shown=0, way=0, program=None, what=None):
            with open(os.path.join(folder, "run.txt"), "w") as file:
                file.write(f"{cycles} 1 {shown} {way}\n")
            return seconds([program or simulate], folder, "2", what)

        if "--check" in arguments:
            ours(1000, 1)
            wanted = open(os.path.join(folder, "outputs.txt")).read()
            for program in (reference, threaded):
                got = subprocess.run([program, "1000", "show"], check=True,
                                     capture_output=True, text=True).stdout
                if wanted != got:
                    sys.exit(f"{name}: Verilator's outputs are not the model's")
            ours(1000, 1, 0, fast)
            if wanted != open(os.path.join(folder, "outputs.txt")).read():
                sys.exit(f"{name}: as code, the outputs are not the model's")
        row = {"name": name, "gates": gates, "their build": their_build, "my build": my_build}
        row["verilator"] = a_cycle(lambda c: seconds([reference, str(c)], folder, "2",
                                                     name + " verilator"))
        # (On the four fast cores, the median of three.)
        row["verilator 4"] = sorted(
            a_cycle(lambda c: seconds([threaded, str(c)], folder, "0-3"))
            for _ in range(3))[1]
        row["code"] = a_cycle(lambda c: ours(c, 0, 0, fast, name + " code"))
        row["rows"] = a_cycle(lambda c: ours(c, 0, 1, None, name + " tables"))
        row["entities"] = a_cycle(lambda c: ours(c, 0, 0))
        row["events"] = a_cycle(lambda c: ours(c, 0, 2), 10)
        row["icarus"] = icarus(name)
        rows.append(row)
        print(f"{name} done", file=sys.stderr, flush=True)

    us = lambda value: f"{1e6 * value:.2f} us"
    print("A clock cycle:\n")
    print("| circuit | ANDs | Verilator | Verilator, 4 threads | as code | as rows | "
          "as entities | by what changed | Icarus Verilog |")
    print("|---|---|---|---|---|---|---|---|---|")
    for row in rows:
        print(f"| {row['name']} | {row['gates']:,} | {us(row['verilator'])} | "
              f"{us(row['verilator 4'])} | {us(row['code'])} | {us(row['rows'])} | "
              f"{us(row['entities'])} | {us(row['events'])} | {us(row['icarus'])} |")
    print("\nGates a second, in millions (the last with the 64 runs counted), "
          "the builds, and the most memory:\n")
    print("| circuit | Verilator | as code | as rows | as entities | as code, 64 runs | "
          "built: Verilator | as code | memory: Verilator | as code | the tables |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for row in rows:
        g, n = row["gates"] / 1e6, row["name"]
        print(f"| {n} | {g / row['verilator']:,.0f} | {g / row['code']:,.0f} | "
              f"{g / row['rows']:,.0f} | {g / row['entities']:,.0f} | "
              f"{64 * g / row['code']:,.0f} | {row['their build']:.0f} s | "
              f"{row['my build']:.0f} s | {PEAK.get(n + ' verilator', 0):.0f} MB | "
              f"{PEAK.get(n + ' code', 0):.0f} MB | {PEAK.get(n + ' tables', 0):.0f} MB |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
