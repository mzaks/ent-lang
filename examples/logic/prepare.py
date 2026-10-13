#!/usr/bin/env python3
"""Makes a circuit ready for the logic simulator and its references.

  examples/logic/fetch.sh                      once: the circuits, into build/
  examples/logic/prepare.py c17 s27 hyp ...    or: prepare.py all

For each circuit named (a file of Verilog among those fetched: ISCAS-85,
ISCAS-89, the EPFL suite; and `cpu`, the PicoRV32 processor inside
examples/logic/cpu.v) Yosys reads it, makes it flat, turns it into
two-input ANDs and inverters with its flip-flops, all starting at 0, and
writes into build/logic/circuits:

  <name>.aag   the netlist the simulator reads (ASCII AIGER)
  <name>.v     the same gates as Verilog, for Icarus Verilog and Verilator

Both come from the one netlist, so all three simulate the same gates.
Says for each circuit how many inputs, flip-flops, outputs and ANDs it
has. Needs yosys on the PATH.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCES = os.path.join(ROOT, "build", "logic", "src")
OUT = os.path.join(ROOT, "build", "logic", "circuits")


def found():
    """Every circuit fetched: its name and its file of Verilog."""
    circuits = {}
    for folder, _, files in os.walk(SOURCES):
        if "best_results" in folder or os.sep + ".git" in folder:
            continue
        for file in files:
            if file.endswith(".v"):
                circuits.setdefault(file[:-2], os.path.join(folder, file))
    # The processor: PicoRV32 inside the module that brings out its bus.
    core = os.path.join(SOURCES, "picorv32", "picorv32.v")
    if os.path.isfile(core):
        circuits = {name: file for name, file in circuits.items()
                    if os.sep + "picorv32" + os.sep not in file}
        circuits["cpu"] = os.path.join(HERE, "cpu.v") + " " + core
    return circuits


def prepare(name, source):
    aag = os.path.join(OUT, name + ".aag")
    verilog = os.path.join(OUT, name + ".v")
    top = "-top cpu" if name == "cpu" else "-auto-top"
    script = f"""
        read_verilog {source}
        hierarchy {top}
        proc; flatten; memory_map; opt -full
        techmap; opt -full
        async2sync; dffunmap
        abc -g AND; opt_clean -purge
        setundef -zero -init
        aigmap; opt_clean -purge
        rename -top {name}
        write_verilog -noattr {verilog}
        write_aiger -ascii -symbols -zinit {aag}
    """
    done = subprocess.run(["yosys", "-q", "-p", script], capture_output=True, text=True)
    if done.returncode:
        sys.exit(f"{name}: yosys failed\n{done.stdout[-2000:]}\n{done.stderr[-2000:]}")
    clock = unclock(aag, verilog)
    with open(aag) as file:
        _, most, inputs, flops, outputs, ands = file.readline().split()
    print(f"{name}: {inputs} inputs, {flops} flip-flops, {outputs} outputs, {ands} ANDs" +
          (f", clock {clock}" if clock else ""))


def unclock(aag, verilog):
    """Takes the clock out of the netlist's inputs, where there is one: the
    flip-flops of the netlist all step together, and nothing reads the
    clock as a value. Gives its name (the Verilog keeps it as a port)."""
    with open(verilog) as file:
        clocks = set(re.findall(r"always @\(posedge ([^)\s]+)\s*\)", file.read()))
    if not clocks:
        return None
    if len(clocks) > 1:
        sys.exit(f"{aag}: more than one clock ({', '.join(sorted(clocks))})")
    clock = clocks.pop().lstrip("\\")
    lines = open(aag).read().split("\n")
    _, most, inputs, flops, outputs, ands = lines[0].split()
    inputs, flops, outputs, ands = int(inputs), int(flops), int(outputs), int(ands)
    body = 1 + inputs + flops + outputs + ands
    named = [line for line in lines[body:] if line.startswith("i") and
             line.split(" ", 1)[1:] == [clock]]
    if len(named) != 1:
        sys.exit(f"{aag}: the clock {clock} is not one of its inputs")
    index = int(named[0].split(" ")[0][1:])
    literal = lines[1 + index].strip()
    for line in lines[1 + inputs:body]:
        if any(int(word) & ~1 == int(literal) for word in line.split()[1 if len(line.split()) > 1 else 0:]):
            sys.exit(f"{aag}: the clock {clock} is read as a value")
    symbols = []
    for line in lines[body:]:
        if line.startswith("i") and line[1:2].isdigit():
            at, rest = line.split(" ", 1)
            if int(at[1:]) == index:
                continue
            if int(at[1:]) > index:
                line = f"i{int(at[1:]) - 1} {rest}"
        symbols.append(line)
    kept = [f"aag {most} {inputs - 1} {flops} {outputs} {ands}"] + \
        [line for at, line in enumerate(lines[1:body]) if at != index] + symbols
    with open(aag, "w") as file:
        file.write("\n".join(kept))
    return clock


def main():
    names = sys.argv[1:]
    circuits = found()
    if not circuits:
        sys.exit("no circuits: run examples/logic/fetch.sh")
    if names == ["all"]:
        names = sorted(circuits)
    os.makedirs(OUT, exist_ok=True)
    for name in names:
        if name not in circuits:
            sys.exit(f"no circuit '{name}' among those fetched")
        prepare(name, circuits[name])
    return 0


if __name__ == "__main__":
    sys.exit(main())
