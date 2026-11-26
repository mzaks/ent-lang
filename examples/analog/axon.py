#!/usr/bin/env python3
"""Writes the circuits of guided axon growth and pruning of Jenderny, Ochs
and Xue ("A memristive circuit for self-organized network topology
formation based on guided axon growth", Scientific Reports 14:16643,
2024) as SPICE netlists:

  examples/analog/axon.py [stop] [name] [currents] [grid] [rram [shift=s] [sigma=s] [seed=n]]
  examples/analog/axon.py 0.05 axon_grow                  # the minimal example, 50 ms
  examples/analog/axon.py 5 axon_prune currents           # growth and pruning, 5 s
  examples/analog/axon.py 0.25 axon_grid_grow grid        # the 15 by 15 grid, its growth
  examples/analog/axon.py 5 axon_grid currents grid       # and pruning, 5 s

into examples/analog/circuits/<name>.cir, for prepare.py.

The minimal example (Fig. 3a and 4): a 2 by 2 grid of Morris-Lecar
oscillators (models/ml.va beside a 2 nF membrane capacitor), each a node
n<x><y>. The start (1,2) is fed rectangular pulses of 2.4 uA, 2 ms on and
4 ms off. Two memsensors grow from it, Ma1 to (2,2) and Ma2 to (1,1); two
synapses (models/synapse.va) lead from those on to the target (2,1). The
target makes the pruning cue (eq. 4a), which every pruning-cue sensor has
as its state, controlled by the target's membrane (models/prune.va; its
second target, d, ground).

The biologically inspired example (Fig. 5): a 15 by 15 grid, nodes
n<x>_<y>, 225 oscillators, a memsensor on every edge but the four of
synapses, 416. The growing neurons N1 and N2 are fed the same pulses; the
targets N1' at (5,3) and N2' at (10,8) have synapses from above and the
right of N1', and from the left and right of N2'. N1 is at (3,15) as Fig.
5a draws it (its path reaches column 5 by two steps right), not at (5,15)
as the text has it; N2 at (15,15). Every pruning-cue sensor hears both
targets, N1' at c and N2' at d.

A memsensor from a to b, growing towards b (Fig. 2b, as its text gives
it: one memristor and one gradient sensor for each direction, the two in
parallel, and the pruning-cue sensor in series; it gives the paper's
800 kOhm before growth, and M0 + Rg0 + Rp0 grown):

  a -- Rp -- m,  m -- M+ -- p -- Rg(+d) -- b,  m -- M- -- q -- Rg(-d) -- b

with M- the other way round (its + node q). Rg(d) = Rg0 + Rg1 (1 -
tanh((d - d1) / d2)), eq. 3b, a resistor here, since the growth cue is
held still. The paper's growth cue maps are shown, not given: here d is
5 along the edges a map's gradient takes the growth (5 is well over the
threshold d1 = 1, and gives the paper's 800 kOhm), and 0 elsewhere (1.8
MOhm, as the paper's other memsensors start). In the minimal example
those are both memsensors; in the grid the paths Fig. 5a draws grown at
0.22 s, each the way it grows (GRID_PATHS), 41 memsensors.

How much of a target's cue reaches a sensor (eq. 4b): 1/2 (s.r /
(|s| |r|^2) + 1) / (1 + |r|), r from the target to the node the memsensor
grows to, s from the target to the synapse to be pruned, at that
synapse's node across from the target (the paper does not place them;
at the middles of the edges the two memsensors of the minimal example
get nearly as much and both are pruned, which its Fig. 4 does not show):
in the minimal example Ma1 1/2, Ma2 1/4. The synapses to be pruned: the
minimal example's from (2,2); the grid's, the ones that should not be,
from N2 into N1' (its right) and from N1 into N2' (its left). eta2 is
0.5 (the minimal example's; the paper gives no other), and Rp1 1.25 MOhm
(models/prune.va says why).

The probes are the membranes (in the grid the starts' and the targets'),
and of each memsensor that grows its nodes m and p; with `currents`,
also the currents through its two gradient sensors, the resistors
(ngspice keeps no current of a compiled model's device): the first is
its growing memristor's, the two together its pruning-cue sensor's, so
its resistance and its memristor's memristance can be worked out
(check/axon.py), with the membranes at its two ends, probed too. (Not
in axon_grow, which check/spice.py holds to ngspice: where a growing
memristor speeds up, ngspice's current at its tightest tolerance,
reltol 1e-6 (at 1e-7 it stops at 16 us, "timestep too small"), is
1.3e-3 to 5e-3 of its range off where tighter tolerances of ours go,
2e-5 apart; ours at the check's 1e-5 is 4e-3 off. The voltages are
within 1e-3.)
Every membrane starts at rest (`.ic`, held there in the operating point,
and its potassium channels as open as there): Table 1's oscillator has
it at -69.4 mV, near EL as the paper says; the operating point from 0 V
finds -26.5 mV, which is no rest (the sodium channel's conductance makes
the node's -63 uS there) and every oscillator would fire once by itself
at the start, growing whatever memsensor has a gradient.
`.options vntol=1e-9`: a membrane at rest under its threshold grows away
from where it is (its sodium channel's conductance is negative there),
and what a step leaves at the run's 1e-6 V comes out a hundred times
larger in when the next spike comes.
"""

import math
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

GRADIENT = 5.0
RG0, RG1, D1, D2 = 5e3, 1.5e6, 1.0, 1.0
PULSES = "PULSE(0 2.4u 0 1u 1u 2m 6m)"
# Table 1's Morris-Lecar oscillator, for its rest.
GL, EL, GNA, ENA, UNA1, UNA2 = 20e-6, -70e-3, 0.2e-3, 50e-3, -1.2e-3, 18e-3
GK, EK, UK1, UK2 = 0.2e-3, -90e-3, 0.0, 10e-3


def opened(u):
    """The share of potassium channels open at u, f(u) of eq. 1b."""
    return (1 + math.tanh((u - UK1) / UK2)) / 2


def rest():
    """The membrane's rest near EL, where its channels' current is 0 with
    as many potassium channels open as stay so there (by halving). Its
    other root, at -26.5 mV, is where an operating point lands from 0 V,
    and it is not at rest: the sodium channel's conductance is so
    negative there that the node's is (-63 uS), and every oscillator
    fires once by itself."""
    def current(u):
        return (GL * (u - EL) + GNA / 2 * (1 + math.tanh((u - UNA1) / UNA2)) * (u - ENA) +
                opened(u) * GK * (u - EK))
    low, high = -0.08, -0.05
    for _ in range(200):
        middle = (low + high) / 2
        if current(low) * current(middle) <= 0:
            high = middle
        else:
            low = middle
    return (low + high) / 2


REST = rest()

# With `rram` (step 3 of the GEMESYS plan): the memsensors' memristors are
# IHP's HfO2 RRAM cells (models/rram.va, from their high resistance, z =
# 1, the gate at 1 V: 12.6 kOhm set, 249 kOhm not), and since a cell
# sets at 0.63 V where an oscillator's spike puts 0.11 V across a
# memsensor, every voltage of the neurons is SCALE times the paper's: the
# oscillators' (EL, ENa, EK, the thresholds and edges, the rest), the
# synapses' (Us1, Us2), the cue's threshold (uth, and the step's width)
# and the pulses' current; then every current is SCALE times too, and
# every time as it was, with the resistors as they are. A spike puts
# 1.1 V across a memsensor. The cells do not drift back as the paper's
# memristors do (they hold their state): a connection is pruned by its
# pruning-cue sensor alone, its cell set.
SCALE = 10.0
# The cells' parameters that spread from cell to cell (models/rram.va's
# values): the gap's scale g0 (the resistances), the voltage uth and the
# field Emin over which a cell switches, and its rate v0.
SPREAD = {"g0": 0.285, "uth": 0.31, "emin": 1.4, "v0": 1e7}


class Cells:
    """How the memristors are made: the paper's (`rram` False), or RRAM
    cells, all shifted by `shift` (0.1: each of SPREAD 10% more) and each
    drawn about that from a normal of `sigma` of it (seeded by `seed`)."""

    def __init__(self, rram=False, shift=0.0, sigma=0.0, seed=1):
        self.rram, self.shift, self.sigma = rram, shift, sigma
        self.random = random.Random(seed)
        self.scale = SCALE if rram else 1.0

    def memristor(self):
        if not self.rram:
            return "growth w0=0"
        values = []
        for name, value in SPREAD.items():
            share = 1 + self.shift + (self.random.gauss(0, self.sigma) if self.sigma else 0)
            values.append(f"{name}={value * max(share, 0.05)!r}")
        return "rram " + " ".join(values) + " w0=1"


def gradient_sensor(d):
    return RG0 + RG1 * (1 - math.tanh((d - D1) / D2))


def factor(target, pruned, at):
    """Eq. 4b: how much of the cue of the target at `target` reaches a
    sensor at `at`, the synapse to be pruned at `pruned`."""
    s = [pruned[0] - target[0], pruned[1] - target[1]]
    r = [at[0] - target[0], at[1] - target[1]]
    rr = r[0] ** 2 + r[1] ** 2
    if rr == 0:
        return 0.5
    cos = (s[0] * r[0] + s[1] * r[1]) / (math.hypot(*s) * rr)
    return 0.5 * (cos + 1) / (1 + math.sqrt(rr))


class Layout:
    """`naming` a node's name by its position; `positions`; `starts`;
    `targets`, each (its position, the position of the synapse to be
    pruned); `synapses` (from, to); `sensors` (name, from, to, gradient);
    the nodes and the memsensors probed."""


def minimal():
    layout = Layout()
    layout.naming = lambda p: f"n{p[0]}{p[1]}"
    layout.positions = [(1, 1), (1, 2), (2, 1), (2, 2)]
    layout.starts = [(1, 2)]
    layout.targets = [((2, 1), (2, 2))]
    layout.synapses = [((1, 1), (2, 1)), ((2, 2), (2, 1))]
    layout.sensors = [("a1", (1, 2), (2, 2), GRADIENT), ("a2", (1, 2), (1, 1), GRADIENT)]
    layout.probed_nodes = layout.positions
    layout.probed = ["a1", "a2"]
    return layout


# The grid's paths as Fig. 5a draws them grown at 0.22 s, each from where
# it grows: N1 down to N1' (and a branch at (5,8) towards N2'), N2 down to
# N2' (and on from (11,8) towards N1').
GRID_PATHS = [
    [(3, 15), (3, 14), (4, 14), (4, 13), (5, 13)] + [(5, y) for y in range(12, 3, -1)],
    [(5, 8), (6, 8), (7, 8), (8, 8), (9, 8)],
    [(15, 15), (15, 14), (15, 13), (15, 12), (14, 12), (14, 11), (14, 10), (14, 9), (14, 8),
     (13, 8), (12, 8), (11, 8)],
    [(11, 8), (11, 7), (11, 6), (10, 6), (9, 6), (8, 6), (8, 5), (7, 5), (7, 4), (7, 3),
     (6, 3)],
]
GRID_STARTS = [(3, 15), (15, 15)]
GRID_TARGETS = [((5, 3), (6, 3)), ((10, 8), (9, 8))]     # N1' prunes from N2, N2' from N1
GRID_SYNAPSES = [((5, 4), (5, 3)), ((6, 3), (5, 3)), ((9, 8), (10, 8)), ((11, 8), (10, 8))]


def grid():
    layout = Layout()
    layout.naming = lambda p: f"n{p[0]}_{p[1]}"
    layout.positions = [(x, y) for x in range(1, 16) for y in range(1, 16)]
    layout.starts = GRID_STARTS
    layout.targets = GRID_TARGETS
    layout.synapses = GRID_SYNAPSES
    synapse_edges = {frozenset(e) for e in GRID_SYNAPSES}
    growing = {}
    for path in GRID_PATHS:
        for a, b in zip(path, path[1:]):
            growing[frozenset((a, b))] = (a, b)
    layout.sensors, layout.probed, ends = [], [], []
    for x in range(1, 16):
        for y in range(1, 16):
            for b in ((x + 1, y), (x, y + 1)):
                if b[0] > 15 or b[1] > 15 or frozenset(((x, y), b)) in synapse_edges:
                    continue
                edge = frozenset(((x, y), b))
                name = f"{x}_{y}_{b[0]}_{b[1]}"
                if edge in growing:
                    a, to = growing[edge]
                    layout.sensors.append((name, a, to, GRADIENT))
                    layout.probed.append(name)
                    ends += [a, to]
                else:
                    layout.sensors.append((name, (x, y), b, 0.0))
    layout.probed_nodes = list(dict.fromkeys(GRID_STARTS + [t for t, _ in GRID_TARGETS] + ends))
    return layout


def netlist(layout, stop, currents, title, cells=None):
    cells = cells or Cells()
    k = cells.scale
    node = layout.naming
    lines = [title]
    neuron = "ml" if k == 1 else (
        f"ml el={EL * k!r} ena={ENA * k!r} una1={UNA1 * k!r} una2={UNA2 * k!r} ek={EK * k!r} "
        f"uk1={UK1 * k!r} uk2={UK2 * k!r}")
    for p in layout.positions:
        lines.append(f"C{node(p)} {node(p)} 0 2n")
        lines.append(f"X{node(p)} {node(p)} 0 {neuron} w0={opened(REST)!r}")
    pulses = PULSES if k == 1 else f"PULSE(0 {2.4e-6 * k!r} 0 1u 1u 2m 6m)"
    for n, p in enumerate(layout.starts):
        lines.append(f"I{n + 1} 0 {node(p)} {pulses}")
    scaled = "" if k == 1 else f" uth={15e-3 * k!r} width={1e-3 * k!r}"
    first = layout.targets[0]
    second = layout.targets[1] if len(layout.targets) > 1 else None
    for name, a, b, d in layout.sensors:
        f1 = factor(first[0], first[1], b)
        if second:
            cue = (f"{node(first[0])} {node(second[0])} prune eta2=0.5 factor={f1!r} "
                   f"factor2={factor(second[0], second[1], b)!r}")
        else:
            cue = f"{node(first[0])} 0 prune eta2=0.5 factor={f1!r}"
        lines.append(f"X{name}p {node(a)} m{name} {cue}{scaled} w0=0")
        lines.append(f"X{name}m m{name} p{name} {cells.memristor()}")
        lines.append(f"R{name}p p{name} {node(b)} {gradient_sensor(d)!r}")
        lines.append(f"X{name}n q{name} m{name} {cells.memristor()}")
        lines.append(f"R{name}n q{name} {node(b)} {gradient_sensor(-d)!r}")
    synapse = "synapse" if k == 1 else f"synapse us1={40e-3 * k!r} us2={1e-3 * k!r}"
    for n, (a, b) in enumerate(layout.synapses):
        lines.append(f"Xs{n + 1} {node(a)} {node(b)} {synapse} w0=0")
    lines.append(".ic " + " ".join(f"v({node(p)})={REST * k!r}" for p in layout.positions))
    lines.append(".options vntol=1e-9")
    lines.append(f".tran 0.1m {stop}")
    lines.append(".print tran " + " ".join(f"v({node(p)})" for p in layout.probed_nodes) + " " +
                 " ".join(f"v(m{name}) v(p{name})" +
                          (f" i(R{name}p) i(R{name}n)" if currents else "")
                          for name in layout.probed))
    lines.append(".end")
    return "\n".join(lines) + "\n"


def main():
    words = sys.argv[1:]
    flags = {w for w in words if w in ("currents", "grid", "rram")}
    settings = dict(w.split("=", 1) for w in words if "=" in w)
    rest = [w for w in words if w not in flags and "=" not in w]
    cells = Cells("rram" in flags, float(settings.get("shift", 0)),
                  float(settings.get("sigma", 0)), int(settings.get("seed", 1)))
    stop = rest[0] if rest else "0.05"
    on_grid = "grid" in flags
    name = rest[1] if len(rest) > 1 else ("axon_grid_grow" if on_grid else "axon_grow")
    layout = grid() if on_grid else minimal()
    title = (f"Guided axon growth and pruning, the biologically inspired example of Jenderny, "
             f"Ochs, Xue 2024 (15 by 15): {stop} s" if on_grid else
             f"Guided axon growth and pruning, the minimal example of Jenderny, Ochs, Xue "
             f"2024: {stop} s")
    path = os.path.join(HERE, "circuits", name + ".cir")
    with open(path, "w") as file:
        file.write(netlist(layout, stop, "currents" in flags, title, cells))
    print(path)


if __name__ == "__main__":
    main()
