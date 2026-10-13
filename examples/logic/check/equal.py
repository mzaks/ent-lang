#!/usr/bin/env python3
"""The model's outputs are Icarus Verilog's, bit for bit (the spec's
checks 2 and 3).

  examples/logic/fetch.sh
  examples/logic/prepare.py all
  examples/logic/check/equal.py [cycles] [circuits]

For each prepared circuit (all of build/logic/circuits, or those named)
both simulate the same gates for so many clock cycles (10,000) with the
same inputs: what the model works out for the first of its 64 runs from
the seed, the input and the cycle is written down here by the same rule
as a file of inputs, which a testbench made here gives to Icarus. Every
output of every cycle is to be the same. Says the first cycle and output
where one is not. Needs iverilog and vvp on the PATH.
"""

import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CIRCUITS = os.path.join(ROOT, "build", "logic", "circuits")
RUN = os.path.join(ROOT, "build", "logic", "run")
MASK = (1 << 64) - 1
SEED = 1


def mix(x):
    x = ((x ^ (x >> 30)) * 0xbf58476d1ce4e5b9) & MASK
    x = ((x ^ (x >> 27)) * 0x94d049bb133111eb) & MASK
    return x ^ (x >> 31)


def drawn(seed, index, cycle):
    """logic.ent's `drawn`: the 64 values of an input in a cycle."""
    return mix(mix((seed + 0x9e3779b97f4a7c15 * (index + 1)) & MASK) ^
               ((cycle * 0xd1342543de82ef95 + 1) & MASK))


def netlist(name):
    """The names of a circuit's inputs and outputs, in the netlist's order."""
    with open(os.path.join(CIRCUITS, name + ".aag")) as file:
        lines = file.read().split("\n")
    _, _, inputs, flops, outputs, gates = lines[0].split()
    inputs, outputs = int(inputs), int(outputs)
    names = {"i": [None] * inputs, "o": [None] * outputs}
    for line in lines[1 + inputs + int(flops) + outputs + int(gates):]:
        if line == "c":
            break
        if line[:1] in names and line[1:2].isdigit():
            at, symbol = line.split(" ", 1)
            names[at[0]][int(at[1:])] = symbol
    return names["i"], names["o"]


def ports(name):
    """The module's ports as its Verilog declares them: for each, how many
    bits, and the lowest bit's number where it has several (else None)."""
    with open(os.path.join(CIRCUITS, name + ".v")) as file:
        text = file.read()
    found = {}
    for kind, wide, port in re.findall(
            r"^\s*(input|output)\s*(\[\s*-?\d+\s*:\s*-?\d+\s*\])?\s*(\\\S+|\w+)\s*;",
            text, re.M):
        found[port.lstrip("\\")] = bool(wide)
    clock = re.findall(r"always @\(posedge ([^)\s]+)\s*\)", text)
    return found, (clock[0].lstrip("\\") if clock else None)


def wire(symbol, declared):
    """A signal of the netlist as the Verilog names it."""
    if symbol in declared and not declared[symbol]:
        return "\\" + symbol + " " if not re.fullmatch(r"\w+", symbol) else symbol
    part = re.fullmatch(r"(.+)\[(\d+)\]", symbol)
    if part and part.group(1) in declared:
        stem = part.group(1)
        stem = "\\" + stem + " " if not re.fullmatch(r"\w+", stem) else stem
        return f"{stem}[{part.group(2)}]"
    if symbol in declared:
        return (("\\" + symbol + " ") if not re.fullmatch(r"\w+", symbol) else symbol) + "[0]"
    sys.exit(f"the netlist's signal '{symbol}' is no port of its Verilog")


def reference(name, cycles):
    """Icarus's outputs: a line of 0s and 1s for each cycle."""
    inputs, outputs = netlist(name)
    declared, clock = ports(name)
    folder = os.path.join(RUN, "icarus_" + name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "inputs.txt"), "w") as file:
        for cycle in range(cycles):
            bits = [drawn(SEED, index, cycle) & 1 for index in range(len(inputs))]
            file.write("".join(str(bit) for bit in reversed(bits)) + "\n")
    many = max(1, len(inputs))
    drives = "\n".join(f"  assign {wire(symbol, declared)} = in[{index}];"
                       for index, symbol in enumerate(inputs))
    shown = ", ".join(wire(symbol, declared) for symbol in outputs)
    widths = {}
    for symbol in inputs + outputs:
        part = re.fullmatch(r"(.+)\[(\d+)\]", symbol)
        if symbol in declared and not declared[symbol]:
            widths.setdefault(symbol, 1)
        elif part and part.group(1) in declared:
            widths[part.group(1)] = max(widths.get(part.group(1), 0), int(part.group(2)) + 1)
        else:
            widths.setdefault(symbol, 1)
    nets = "\n".join(
        f"  wire {'[%d:0] ' % (wide - 1) if declared.get(port) else ''}"
        f"{port if re.fullmatch(chr(92) + 'w+', port) else chr(92) + port + ' '};"
        for port, wide in widths.items())
    held = [port for port in declared if port != clock]
    joins = ", ".join(
        f".{p if re.fullmatch(chr(92) + 'w+', p) else chr(92) + p + ' '}"
        f"({p if re.fullmatch(chr(92) + 'w+', p) else chr(92) + p + ' '})" for p in held)
    if clock:
        joins += (", " if joins else "") + f".{clock}(clk)"
    bench = f"""`timescale 1ns/1ns
module bench;
  reg clk = 0;
  reg [{many - 1}:0] in;
  reg [{many - 1}:0] all [0:{cycles - 1}];
{nets}
{drives}
  {name} dut({joins});
  integer cycle;
  initial begin
    $readmemb("inputs.txt", all);
    for (cycle = 0; cycle < {cycles}; cycle = cycle + 1) begin
      in = all[cycle];
      #1;
      $display("%b", {{{shown}}});
      clk = 1;
      #1;
      clk = 0;
    end
    $finish;
  end
endmodule
"""
    with open(os.path.join(folder, "bench.v"), "w") as file:
        file.write(bench)
    subprocess.run(["iverilog", "-g2005", "-o", "bench.vvp", "bench.v",
                    os.path.join(CIRCUITS, name + ".v")], check=True, cwd=folder)
    said = subprocess.run(["vvp", "-n", "bench.vvp"], check=True, capture_output=True,
                          text=True, cwd=folder).stdout
    shutil.rmtree(folder)
    return [line for line in said.split("\n") if line and set(line) <= set("01xz")]


def model(program, name, cycles):
    folder = os.path.join(RUN, "model_" + name)
    os.makedirs(folder, exist_ok=True)
    shutil.copy(os.path.join(CIRCUITS, name + ".aag"), os.path.join(folder, "circuit.aag"))
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write(f"{cycles} {SEED}\n")
    subprocess.run([program], check=True, capture_output=True, cwd=folder)
    with open(os.path.join(folder, "outputs.txt")) as file:
        lines = file.read().split("\n")
    shutil.rmtree(folder)
    return [line for line in lines if line]


def main():
    arguments = sys.argv[1:]
    cycles = next((int(a) for a in arguments if a.isdigit()), 10000)
    names = [a for a in arguments if not a.isdigit()] or sorted(
        name[:-4] for name in os.listdir(CIRCUITS) if name.endswith(".aag"))
    os.makedirs(RUN, exist_ok=True)
    program = os.path.join(RUN, "simulate")
    environment = dict(os.environ)
    environment.setdefault("ENT_BIN", os.path.join(ROOT, "build", "bin"))
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ent"), "build",
                    "-I", os.path.join(HERE, ".."),
                    os.path.join(HERE, "..", "simulate.ent"), "-o", program],
                   check=True, env=environment)
    failed = 0
    for name in names:
        wanted = reference(name, cycles)
        got = model(program, name, cycles)
        differ = next((cycle for cycle, (a, b) in enumerate(zip(wanted, got)) if a != b), None)
        if differ is None and len(wanted) == len(got) == cycles:
            print(f"{name}: the same {cycles} cycles, {len(wanted[0]) if wanted else 0} outputs")
            continue
        failed += 1
        if differ is None:
            print(f"{name}: NOT the same: {len(wanted)} cycles from Icarus, "
                  f"{len(got)} from the model")
        else:
            at = next(i for i, (a, b) in enumerate(zip(wanted[differ], got[differ])) if a != b)
            print(f"{name}: NOT the same from cycle {differ}, output {at}: "
                  f"Icarus {wanted[differ][at]}, the model {got[differ][at]}")
    print(f"{len(names)} circuits, {failed} not the same")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
