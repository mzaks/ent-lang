#!/usr/bin/env python3
"""The operating point is the circuit's own, to 1e-12, and the
transients are what backward Euler makes of the circuit's own (the
spec's check 2).

  examples/analog/prepare.py all
  examples/analog/crossbar.py 4 1 biolek 0.1 1 write   (and others)
  examples/analog/check/exact.py [circuits]

For each prepared circuit small enough (up to 80 unknowns) the equations
of its operating point are solved here exactly, in fractions: Kirchhoff's
current law at every node but ground, with GMIN to ground from each, and
a voltage source's two nodes as far apart as it says; a capacitor open, a
memristor the resistor its first state makes it (Biolek's and VTEAM's;
one of Yakopcic's, whose current is a sinh, leaves its circuit out). Every probe of way 0
(direct) is to be that, rounded, to within 1e-12 of it (and of 1e-15 V
or A, for a value of 0). But for a crossbar whose wires have less than
1 ohm: to 1e-11. Wires of 10 siemens beside memristors of 1e-4 make the
equations hard to solve in f64, and the sense resistors' voltages come
out a few parts in 10^12 off (a step of refinement with the same
factors does not change that). Ways 1 and 2 are held against way 0 by
check/agree.py. The circuit runs without its `tran` here, so only its
operating point: a crossbar's at 50 ms, where its pulses are high (its
memristors in their first states); every other circuit's at 0. Says the first probe that is not.

Then the transients of the two RC low-passes, whose waves have closed
forms: rc_step (a step, with its rise of 1 us, into 1k and 1 uF) and
rc_sine (a sine at the corner frequency). Way 0 runs each at a fixed
step, the circuit's, half of it and a quarter, by backward Euler, and
rc_sine by the trapezoidal rule too. The error, the largest over the run
and all probes as a share of the probe's range, is to be a quarter at a
quarter of the step by backward Euler (3 to 5 times smaller) and a
sixteenth by the trapezoidal rule (12 to 20 times), and below 1% at the
circuit's step. (Not rc_step by the trapezoidal rule at a fixed step:
its edge falls inside a step, where no rule is of the second order, and
the trapezoidal rule rings there; a corner of a source is a time point
when the steps are chosen, below.)
(Over two halvings, not each: rc_step's edge, 1 us, falls between two
time points at another place for every step, which moves the error of
each halving up or down. The trapezoidal rule takes its first step by
backward Euler, as SPICE does, whose error is of the second order then.)
Then the same two by the steps chosen by their error (milestone M4),
within 1e-3 of the range.
"""

import math

import os
import sys
from fractions import Fraction

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

GMIN = Fraction(1, 10 ** 12)


def when(name):
    return 0.05 if name.startswith("crossbar") else 0.0


def resistance(model, numbers):
    """A memristor's resistance in its first state, exactly, or None for a
    model whose current is no straight line (Yakopcic's)."""
    x = [Fraction(n) for n in numbers]
    if model == "biolek":
        ron, roff, w = x[0], x[1], x[5]
        return ron * w + roff * (1 - w)
    if model == "vteam":
        ron, roff, w_on, w_off, w = x[0], x[1], x[8], x[9], x[13]
        return ron + (roff - ron) * (w - w_on) / (w_off - w_on)
    return None


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
                g = 1 / resistance(wave, numbers)
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
            values.append((v(a) - v(b)) / resistance(wave, numbers))
        elif device == "C":
            values.append(Fraction(0))
        elif device == "V":
            values.append(x[net.nodes - 1 + branches.index(which)])
        else:
            values.append(Fraction(common.level(wave, numbers, t)))
    return values


def rc_step(t):
    """v(out) and i(V1) of rc_step: the step rises from 1 ms over 1 us."""
    tau, delay, rise, r = 1e-3, 1e-3, 1e-6, 1e3
    if t <= delay:
        u = v = 0.0
    elif t <= delay + rise:
        s = t - delay
        u = s / rise
        v = (s - tau * (1 - math.exp(-s / tau))) / rise
    else:
        top = (rise - tau * (1 - math.exp(-rise / tau))) / rise
        u = 1.0
        v = 1 - (1 - top) * math.exp(-(t - delay - rise) / tau)
    return [v, (v - u) / r]


def rc_sine(t):
    """v(in) and v(out) of rc_sine."""
    tau, w = 1e-3, 2 * math.pi * 159.155
    u = math.sin(w * t)
    v = (math.sin(w * t) - w * tau * math.cos(w * t) + w * tau * math.exp(-t / tau)) / \
        (1 + (w * tau) ** 2)
    return [u, v]


def error(waves, exact):
    """The largest error over the run and all probes, as a share of the
    probe's range."""
    wanted = [exact(line[0]) for line in waves]
    worst = 0.0
    for k in range(len(wanted[0])):
        column = [w[k] for w in wanted]
        range_ = max(column) - min(column)
        worst = max(worst, max(abs(line[1 + k] - w[k]) for line, w in zip(waves, wanted)) / range_)
    return worst


def transients(program):
    failed = 0
    for name, exact in (("rc_step", rc_step), ("rc_sine", rc_sine)):
        if not os.path.isdir(os.path.join(common.CIRCUITS, name)):
            continue
        net = common.Netlist(name)
        rules = [(0, "backward Euler", 3, 5)] + \
            ([(1, "trapezoidal", 12, 20)] if name == "rc_sine" else [])
        for method, rule, low, high in rules:
            errors = [error(common.run(program, name, 0, step=net.tran[0] / share,
                                       method=method)[0], exact) for share in (1, 2, 4)]
            ratios = [a / b for a, b in zip(errors, errors[1:])]
            good = errors[0] < 0.01 and low <= errors[0] / errors[2] <= high
            failed += 0 if good else 1
            print(f"{name}, {rule}: error {', '.join(f'{e:.2e}' for e in errors)} of the range "
                  f"at steps h, h/2, h/4 (ratios {', '.join(f'{r:.2f}' for r in ratios)}; over "
                  f"both {errors[0] / errors[2]:.2f})" + ("" if good else ": NOT as it should be"))
        waves, said = common.run(program, name, 0)
        chosen = error(waves, exact)
        good = chosen <= 1e-3
        failed += 0 if good else 1
        print(f"{name}, steps chosen: {len(waves)} time points, error {chosen:.2e} of the range" +
              ("" if good else ": NOT as it should be"))
    return failed


def main():
    names = sys.argv[1:] or sorted(os.listdir(common.CIRCUITS))
    program = common.build()
    failed = checked = 0
    for name in names:
        net = common.Netlist(name)
        size = net.nodes - 1 + sum(1 for d in net.devices if d[0] == "V")
        if size > 80 or any(d[0] == "M" and resistance(d[3], d[4]) is None
                             for d in net.devices):
            continue
        checked += 1
        waves, said = common.run(program, name, 0, when(name), operating=True)
        if not waves:
            failed += 1
            print(f"{name}: no waves ({said})")
            continue
        line = waves[0]
        t = line[0]
        x, branches = solve(net, t)
        wanted = probes(net, x, branches, t)
        got = line[1:]
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
    if not sys.argv[1:]:
        failed += transients(program)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
