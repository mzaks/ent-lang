#!/usr/bin/env python3
"""Writes an N by N crossbar of memristors as a SPICE netlist (spec 9.2).

  examples/analog/crossbar.py N R_wire model variation seed [read|write]
  examples/analog/crossbar.py 16 1 biolek 0.1 1 write

into build/analog/sources/crossbar_<N>_<R_wire>_<model>_<scenario>.cir
(with the variation and seed in its title), for prepare.py.

Row i is driven by a grounded source at its left end (node d<i>); column
j ends in a sense resistor to ground at its foot (node s<j>, 1k). A
memristor sits at every crosspoint, from row i to column j. With wire
resistance, every row and column is a line of resistors of R_wire ohms:
from the driver to the first crosspoint and from each crosspoint to
the next along the row (nodes r<i>_<j>), and down the column from each
crosspoint to the next and from the last to the sense resistor (nodes
c<j>_<i>). With R_wire 0 there are no wires: every memristor of row i
joins d<i> straight to s<j>.

Scenarios: `read` drives every row with 0.1 V pulses that hardly move a
state; `write` (the default) drives every fourth row, from row 0, with
1.2 V pulses that do, and holds the others at 0.

What each memristor starts with and its parameters are drawn from the
seed, the row and the column, by this rule (which check scripts that
need it repeat): u(k) = the k-th draw, mix(mix(seed + G*(i*N + j + 1))
^ (k*H + 1)) / 2^64, with logic.ent's `mix` (splitmix64's finaliser),
G = 0x9e3779b97f4a7c15 and H = 0xd1342543de82ef95. Then w0 = 0.1 +
0.8*u(0), and each resistance of the model (Ron, Roff) is its default
times 1 + variation*(2*u(k) - 1), k = 1, 2.

The probes are the sense nodes, and the first row's driver current.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SOURCES = os.path.join(ROOT, "build", "analog", "sources")
MASK = (1 << 64) - 1
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import prepare  # the models' parameters


def mix(x):
    x = ((x ^ (x >> 30)) * 0xbf58476d1ce4e5b9) & MASK
    x = ((x ^ (x >> 27)) * 0x94d049bb133111eb) & MASK
    return x ^ (x >> 31)


def drawn(seed, n, i, j, k):
    """The k-th draw for the memristor of row i and column j, in [0, 1)."""
    first = mix((seed + 0x9e3779b97f4a7c15 * (i * n + j + 1)) & MASK)
    return mix(first ^ ((k * 0xd1342543de82ef95 + 1) & MASK)) / 2.0 ** 64


def write(n, wire, model, variation, seed, scenario):
    defaults = dict(prepare.parameters(model))
    varied = [name for name in ("Ron", "Roff") if name in defaults]
    name = f"crossbar_{n}_{prepare.text(wire)}_{model}_{scenario}"
    lines = [f"A {n} by {n} crossbar of {model} memristors, wires of {prepare.text(wire)} ohm, "
             f"{scenario}, variation {variation}, seed {seed}"]
    level = 1.2 if scenario == "write" else 0.1
    for i in range(n):
        driven = scenario == "read" or i % 4 == 0
        lines.append(f"V{i} d{i} 0 " + (f"PULSE(0 {level} 10m 1m 1m 100m 300m)" if driven
                                         else "DC 0"))
    for j in range(n):
        lines.append(f"Rs{j} s{j} 0 1k")
    for i in range(n):
        for j in range(n):
            row = f"r{i}_{j}" if wire else f"d{i}"
            column = f"c{j}_{i}" if wire else f"s{j}"
            settings = [f"w0={0.1 + 0.8 * drawn(seed, n, i, j, 0)!r}"]
            for k, parameter in enumerate(varied, 1):
                scale = 1 + variation * (2 * drawn(seed, n, i, j, k) - 1)
                settings.append(f"{parameter}={defaults[parameter] * scale!r}")
            lines.append(f"X{i}_{j} {row} {column} {model} " + " ".join(settings))
    if wire:
        for i in range(n):
            lines.append(f"Rr{i}_0 d{i} r{i}_0 {prepare.text(wire)}")
            for j in range(1, n):
                lines.append(f"Rr{i}_{j} r{i}_{j - 1} r{i}_{j} {prepare.text(wire)}")
        for j in range(n):
            for i in range(1, n):
                lines.append(f"Rc{j}_{i} c{j}_{i - 1} c{j}_{i} {prepare.text(wire)}")
            lines.append(f"Rc{j}_{n} c{j}_{n - 1} s{j} {prepare.text(wire)}")
    lines.append(".tran 1m 400m")
    lines.append(".print tran " + " ".join(f"v(s{j})" for j in range(n)) + " i(V0)")
    lines.append(".end")
    os.makedirs(SOURCES, exist_ok=True)
    with open(os.path.join(SOURCES, name + ".cir"), "w") as file:
        file.write("\n".join(lines) + "\n")
    print(name)


def main():
    arguments = sys.argv[1:]
    if len(arguments) not in (5, 6):
        sys.exit(__doc__)
    n, wire, model = int(arguments[0]), float(arguments[1]), arguments[2]
    variation, seed = float(arguments[3]), int(arguments[4])
    scenario = arguments[5] if len(arguments) > 5 else "write"
    if scenario not in ("read", "write") or n < 1 or wire < 0:
        sys.exit(__doc__)
    if prepare.parameters(model) is None:
        sys.exit(f"no model {model} in {prepare.MODELS}")
    write(n, wire, model, variation, seed, scenario)
    return 0


if __name__ == "__main__":
    sys.exit(main())
