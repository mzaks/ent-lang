#!/usr/bin/env python3
"""The operating point is the circuit's own, to 1e-12 (the spec's check
2, its DC part).

  examples/analog/prepare.py all
  examples/analog/crossbar.py 4 1 biolek 0.1 1 write   (and others)
  examples/analog/check/exact.py [circuits]

For each prepared circuit small enough (up to 80 unknowns) the equations
of its operating point are solved here exactly, in fractions: Kirchhoff's
current law at every node but ground, with GMIN to ground from each, and
a voltage source's two nodes as far apart as it says; a capacitor open, a
memristor the resistor its first state makes it. Every probe of way 0
(direct) is to be that, rounded, to within 1e-12 of it (and of 1e-15 V
or A, for a value of 0). But for a crossbar whose wires have less than
1 ohm: to 1e-11. Wires of 10 siemens beside memristors of 1e-4 make the
equations hard to solve in f64, and the sense resistors' voltages come
out a few parts in 10^12 off (a step of refinement with the same
factors does not change that). Ways 1 and 2 are held against way 0 by
check/agree.py. A crossbar's operating point is taken at 50 ms, where its
pulses are high; every other's at 0. Says the first probe that is not.
"""

import os
import sys
from fractions import Fraction

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

GMIN = Fraction(1, 10 ** 12)


def when(name):
    return 0.05 if name.startswith("crossbar") else 0.0


def solve(net, t):
    """The exact unknowns: the nodes' voltages but ground's, then the
    voltage sources' currents from a to b."""
    branches = [k for k, device in enumerate(net.devices) if device[0] == "V"]
    size = net.nodes - 1 + len(branches)
    matrix = [[Fraction(0)] * (size + 1) for _ in range(size)]

    def add(row, col, value):
        if row >= 0 and col >= 0:
            matrix[row][col] += value

    for node in range(1, net.nodes):
        add(node - 1, node - 1, GMIN)
    for k, (kind, a, b, wave, numbers) in enumerate(net.devices):
        if kind in "RM":
            if kind == "R":
                g = 1 / Fraction(numbers[0])
            else:
                ron, roff, w = (Fraction(x) for x in (numbers[0], numbers[1], numbers[5]))
                g = 1 / (ron * w + roff * (1 - w))
            add(a - 1, a - 1, g)
            add(b - 1, b - 1, g)
            add(a - 1, b - 1, -g)
            add(b - 1, a - 1, -g)
        elif kind == "V":
            m = net.nodes - 1 + branches.index(k)
            add(a - 1, m, 1)
            add(m, a - 1, 1)
            add(b - 1, m, -1)
            add(m, b - 1, -1)
            matrix[m][size] += Fraction(common.level(wave, numbers, t))
        elif kind == "I":
            value = Fraction(common.level(wave, numbers, t))
            if a > 0:
                matrix[a - 1][size] -= value
            if b > 0:
                matrix[b - 1][size] += value
    # Gauss–Jordan, in fractions.
    for col in range(size):
        pivot = next(r for r in range(col, size) if matrix[r][col] != 0)
        matrix[col], matrix[pivot] = matrix[pivot], matrix[col]
        lead = matrix[col][col]
        matrix[col] = [x / lead for x in matrix[col]]
        for r in range(size):
            if r != col and matrix[r][col] != 0:
                factor = matrix[r][col]
                matrix[r] = [x - factor * y for x, y in zip(matrix[r], matrix[col])]
    return [matrix[r][size] for r in range(size)], branches


def probes(net, x, branches, t):
    def v(node):
        return Fraction(0) if node == 0 else x[node - 1]
    values = []
    for kind, which in net.probes:
        if kind == "v":
            values.append(v(which))
            continue
        device, a, b, wave, numbers = net.devices[which]
        if device == "R":
            values.append((v(a) - v(b)) / Fraction(numbers[0]))
        elif device == "M":
            ron, roff, w = (Fraction(y) for y in (numbers[0], numbers[1], numbers[5]))
            values.append((v(a) - v(b)) / (ron * w + roff * (1 - w)))
        elif device == "C":
            values.append(Fraction(0))
        elif device == "V":
            values.append(x[net.nodes - 1 + branches.index(which)])
        else:
            values.append(Fraction(common.level(wave, numbers, t)))
    return values


def main():
    names = sys.argv[1:] or sorted(os.listdir(common.CIRCUITS))
    program = common.build()
    failed = checked = 0
    for name in names:
        net = common.Netlist(name)
        size = net.nodes - 1 + sum(1 for d in net.devices if d[0] == "V")
        if size > 80:
            continue
        checked += 1
        t = when(name)
        x, branches = solve(net, t)
        wanted = probes(net, x, branches, t)
        waves, said = common.run(program, name, 0, t)
        if not waves:
            failed += 1
            print(f"{name}: no waves ({said})")
            continue
        got = waves[0][1:]
        wire = name.split("_")[2] if name.startswith("crossbar_") else ""
        relative = Fraction(1, 10 ** 11) if wire and 0 < float(wire) < 1 else \
            Fraction(1, 10 ** 12)
        worst, at = 0.0, None
        for k, (want, value) in enumerate(zip(wanted, got)):
            exact = float(want)
            error = abs(Fraction(value) - want)
            allowed = relative * abs(want) + Fraction(1, 10 ** 15)
            share = float(error / allowed)
            if share > worst:
                worst, at = share, (k, exact, value)
        if worst > 1:
            failed += 1
            k, exact, value = at
            print(f"{name}: NOT exact: probe {k} is {value!r}, to be {exact!r}")
        else:
            print(f"{name}: {len(got)} probes exact to {float(relative):g} "
                  f"(the worst at {worst:.3f} of that), at {t} s")
    print(f"{checked} circuits, {failed} not exact")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
