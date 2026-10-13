#!/usr/bin/env python3
"""Times the processor of examples/logic against Verilator.

  examples/logic/fetch.sh
  examples/logic/prepare.py cpu
  python3 bench/logic/cpu.py

PicoRV32 runs Dhrystone to its end (583,031 clock cycles) with its
memory outside the circuit, which answers the bus a cycle after it is
asked: in the model that is examples/logic/bus.ent, for Verilator a
harness in C++ made here that answers alike. Verilator runs twice: the
gates Yosys made of the processor, which are what the model runs, and
the processor's Verilog as it was written (examples/logic/cpu.v around
picorv32.v), which is what somebody with that Verilog would give it.

First every output of every cycle and what the program says are held
against the model's (the gates written out as code). Then each runs
without writing its outputs: one core (CPU 2), native builds, the whole
run with its start, the least of three.
"""

import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LOGIC = os.path.join(ROOT, "examples", "logic")
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
BENCH = os.path.join(ROOT, "build", "logic", "bench_cpu")
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(LOGIC, "check"))
import cpu as check  # the program, the pins, and the model's runs
import equal

HARNESS = r"""
#include "Vcpu.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static uint64_t mix(uint64_t x) {
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

int main(int argc, char **argv) {
  long cycles = atol(argv[1]);
  bool shown = argc > 3;
  std::vector<uint32_t> memory(65536, 0);
  FILE *image = fopen(argv[2], "rb");
  int byte;
  for (long at = 0; image && (byte = fgetc(image)) != EOF; ++at)
    if ((0x10000 + at) / 4 < 65536)
      memory[(0x10000 + at) / 4] |= (uint32_t)byte << ((at % 4) * 8);
  if (image)
    fclose(image);
  FILE *said = fopen("console.txt", "wb");
  Vcpu *top = new Vcpu;
  bool resetn = false, ready = false, trapped = false;
  uint32_t rdata = 0;
  uint64_t sum = 0;
  long cycle = 0;
  top->clk = 0;
  for (; cycle < cycles && !trapped; ++cycle) {
    top->resetn = resetn;
    top->mem_ready = ready;
    top->mem_rdata = rdata;
    top->eval();
    bool outputs[] = {OUTPUTS};
    for (bool bit : outputs) {
      sum = mix(sum ^ (bit ? ~0ull : 0ull));
      if (shown)
        putchar('0' + bit);
    }
    if (shown)
      putchar('\n');
    bool answered = ready;
    ready = false;
    if (top->mem_valid && !answered) {
      ready = true;
      uint32_t address = top->mem_addr, at = address / 4;
      if (top->mem_wstrb == 0) {
        rdata = at < 65536 ? memory[at] : 0;
      } else if (address == 0x10000000u) {
        fputc(top->mem_wdata & 0xff, said);
      } else if (at < 65536) {
        uint32_t mask = 0;
        for (int k = 0; k < 4; ++k)
          if ((top->mem_wstrb >> k) & 1)
            mask |= 0xffu << (8 * k);
        memory[at] = (memory[at] & ~mask) | (top->mem_wdata & mask);
      }
    }
    if (resetn && top->trap)
      trapped = true;
    resetn = cycle + 1 >= 10;
    top->clk = 1;
    top->eval();
    top->clk = 0;
  }
  fclose(said);
  if (!shown)
    printf("cycles %ld sum %llu\n", cycle, (unsigned long long)sum);
  delete top;
  return 0;
}
"""


def verilator(name, sources):
    """Verilator's program for the processor from these files of Verilog."""
    folder = os.path.join(BENCH, name)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    _, outputs = equal.netlist("cpu")
    bits = []
    for symbol in outputs:
        port, _, at = symbol.partition("[")
        bits.append(f"(bool)((top->{port} >> {at.rstrip(']')}) & 1)" if at else
                    f"(bool)top->{port}")
    with open(os.path.join(folder, "harness.cpp"), "w") as file:
        file.write("#define OUTPUTS " + ", ".join(bits) + "\n" + HARNESS)
    started = time.time()
    subprocess.run(
        ["verilator", "--cc", "--exe", "--build", "-j", "0", "-O3", "--x-assign", "fast",
         "--x-initial", "fast", "--noassert", "-Wno-fatal", "--top-module", "cpu",
         "-CFLAGS", "-O3 -march=native"] + sources + ["harness.cpp"],
        check=True, cwd=folder, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(folder, "obj_dir", "Vcpu"), time.time() - started


def model(source, built, more=()):
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    environment.setdefault("ENT_CFLAGS", "-march=native")
    environment["ENT_CACHE"] = os.path.join(BENCH, "no_cache")
    shutil.rmtree(environment["ENT_CACHE"], ignore_errors=True)
    started = time.time()
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build", "-I", LOGIC,
                    *more, os.path.join(LOGIC, source), "-o", built],
                   check=True, env=environment)
    return built, time.time() - started


def least(command, folder):
    """The least of three runs' seconds, and what the last said."""
    times = []
    for _ in range(3):
        started = time.time()
        said = subprocess.run(["taskset", "-c", "2"] + command, check=True, cwd=folder,
                              capture_output=True, text=True).stdout
        times.append(time.time() - started)
    return min(times), said.strip().split("\n")[-1]


def main():
    os.makedirs(BENCH, exist_ok=True)
    check.RUN = BENCH
    image = check.firmware()
    subprocess.run([sys.executable, os.path.join(LOGIC, "compile.py"), "cpu"], check=True,
                   stdout=subprocess.DEVNULL)
    coded, coded_build = model("cpu_coded.ent", os.path.join(BENCH, "cpu_coded"),
                               ["-I", os.path.join(ROOT, "build", "logic", "code", "cpu")])
    tables, _ = model("cpu.ent", os.path.join(BENCH, "cpu"))
    gates, gates_build = verilator("verilator_gates", [os.path.join(CIRCUITS, "cpu.v")])
    written, written_build = verilator(
        "verilator_written",
        [os.path.join(LOGIC, "cpu.v"),
         os.path.join(ROOT, "build", "logic", "src", "picorv32", "picorv32.v")])

    # What the model gives, cycle by cycle, and what each Verilator gives.
    lines, console, said = check.model(coded, image, 5000000, 0, "coded")
    cycles = len(lines)
    for name, program in (("the gates", gates), ("the Verilog as written", written)):
        folder = os.path.dirname(os.path.dirname(program))
        out = subprocess.run([program, "5000000", image, "show"], check=True,
                             capture_output=True, text=True, cwd=folder).stdout
        theirs = [line for line in out.split("\n") if line]
        with open(os.path.join(folder, "console.txt"), errors="replace") as file:
            same = theirs == lines and file.read() == console
        print(f"Verilator, {name}: {'the same' if same else 'NOT the same'} "
              f"{len(theirs)} cycles as the model's {cycles}")
        if not same:
            return 1

    rows = []
    for name, program, build in (("Verilator, the Verilog as written", written, written_build),
                                 ("Verilator, the gates", gates, gates_build)):
        folder = os.path.dirname(os.path.dirname(program))
        seconds, last = least([program, "5000000", image], folder)
        rows.append((name, seconds, build, last))
    for name, program, way, folder, build in (
            ("the model, as code", coded, 0, "coded", coded_build),
            ("the model, as entities, those whose inputs changed", tables, 2, "events", None),
            ("the model, as rows", tables, 1, "rows", None),
            ("the model, as entities, all", tables, 0, "entities", None)):
        check.model(program, image, 2000, way, folder)   # the folder and its files
        with open(os.path.join(BENCH, folder, "run.txt"), "w") as file:
            file.write(f"5000000 1 0 {way}\n")
        seconds, last = least([program], os.path.join(BENCH, folder))
        rows.append((name, seconds, build, last))
    print("| | the run | a cycle | cycles a second | built in |")
    print("|---|---|---|---|---|")
    for name, seconds, build, last in rows:
        print(f"| {name} | {seconds:.2f} s | {1e6 * seconds / cycles:.2f} us | "
              f"{cycles / seconds:,.0f} | {'%.0f s' % build if build else ''} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
