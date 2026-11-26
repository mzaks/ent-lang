#!/usr/bin/env python3
"""Writes the minimal example of guided axon growth and pruning of
Jenderny, Ochs and Xue ("A memristive circuit for self-organized network
topology formation based on guided axon growth", Scientific Reports
14:16643, 2024, Fig. 3a and 4) as a SPICE netlist:

  examples/analog/axon.py [stop] [name] [currents]
  examples/analog/axon.py 0.05 axon_grow             # the growth, 50 ms
  examples/analog/axon.py 5 axon_prune currents      # growth and pruning, 5 s

into examples/analog/circuits/<name>.cir, for prepare.py.

A 2 by 2 grid of Morris-Lecar oscillators (models/ml.va beside a 2 nF
membrane capacitor), each a node n<x><y>. The start (1,2) is fed
rectangular pulses of 2.4 uA, 2 ms on and 4 ms off. Two memsensors grow
from it, Ma1 to (2,2) and Ma2 to (1,1); two synapses (models/synapse.va)
lead from those on to the target (2,1). The target makes the pruning cue
(eq. 4a), which every pruning-cue sensor has as its state, controlled by
the target's membrane (models/prune.va).

A memsensor from a to b, growing towards b (Fig. 2b, as its text gives
it: one memristor and one gradient sensor for each direction, the two in
parallel, and the pruning-cue sensor in series; it gives the paper's
800 kOhm before growth, and M0 + Rg0 + Rp0 grown):

  a -- Rp -- m,  m -- M+ -- p -- Rg(+d) -- b,  m -- M- -- q -- Rg(-d) -- b

with M- the other way round (its + node q). Rg(d) = Rg0 + Rg1 (1 -
tanh((d - d1) / d2)), eq. 3b, a resistor here, since the growth cue is
held still; d = 5 along both memsensors (the paper's map, a gradient
towards the target, is shown, not given; 5 is well over its threshold
d1 = 1).

How much of the target's cue reaches a sensor (eq. 4b): 1/2 (s.r /
(|s| |r|^2) + 1) / (1 + |r|), r from the target to the node the memsensor
grows to, s from the target to the synapse to be pruned, Ms2's, at its
node (2,2) (the paper does not place them; at the middles of the edges
the two memsensors get nearly as much and both are pruned, which its
Fig. 4 does not show): Ma1 1/2, Ma2 1/4. The minimal example's eta2 is
0.5, and Rp1 1.25 MOhm (models/prune.va says why).

The probes are the four membranes and of each memsensor its nodes m and
p; with `currents`, also the currents through its two gradient sensors,
the resistors (ngspice keeps no current of a compiled model's device):
the first is its growing memristor's, the two together its pruning-cue
sensor's, so its resistance and its memristor's memristance can be
worked out (check/axon.py). (Not in axon_grow, which check/spice.py
holds to ngspice: where a growing memristor speeds up, ngspice's
current at its tightest tolerance, reltol 1e-6 (at 1e-7 it stops at 16
us, "timestep too small"), is 1.3e-3 to 5e-3 of its range off where
tighter tolerances of ours go, 2e-5 apart; ours at the check's 1e-5 is
4e-3 off. The voltages are within 1e-3.)
`.options vntol=1e-9`: a membrane at rest under its threshold grows away
from where it is (its sodium channel's conductance is negative there),
and what a step leaves at the run's 1e-6 V comes out a hundred times
larger in when the next spike comes.
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

POSITIONS = {"n11": (1, 1), "n12": (1, 2), "n21": (2, 1), "n22": (2, 2)}
START, TARGET = "n12", "n21"
SENSORS = [("a1", "n12", "n22"), ("a2", "n12", "n11")]
SYNAPSES = [("s1", "n11", "n21"), ("s2", "n22", "n21")]
PRUNED = "n22"          # the node of the synapse whose axon is to be pruned
GRADIENT = 5.0
RG0, RG1, D1, D2 = 5e3, 1.5e6, 1.0, 1.0


def gradient_sensor(d):
    return RG0 + RG1 * (1 - math.tanh((d - D1) / D2))


def factor(node):
    x0, y0 = POSITIONS[TARGET]
    s = [POSITIONS[PRUNED][0] - x0, POSITIONS[PRUNED][1] - y0]
    r = [POSITIONS[node][0] - x0, POSITIONS[node][1] - y0]
    rr = r[0] ** 2 + r[1] ** 2
    if rr == 0:
        return 0.5
    cos = (s[0] * r[0] + s[1] * r[1]) / (math.hypot(*s) * rr)
    return 0.5 * (cos + 1) / (1 + math.sqrt(rr))


def netlist(stop, currents):
    lines = [f"Guided axon growth and pruning, the minimal example of Jenderny, Ochs, Xue "
             f"2024: {stop} s"]
    for node in POSITIONS:
        lines.append(f"C{node} {node} 0 2n")
        lines.append(f"X{node} {node} 0 ml w0=0")
    lines.append(f"I1 0 {START} PULSE(0 2.4u 0 1u 1u 2m 6m)")
    for name, a, b in SENSORS:
        lines.append(f"X{name}p {a} m{name} {TARGET} prune eta2=0.5 factor={factor(b)!r} w0=0")
        lines.append(f"X{name}m m{name} p{name} growth w0=0")
        lines.append(f"R{name}p p{name} {b} {gradient_sensor(GRADIENT)!r}")
        lines.append(f"X{name}n q{name} m{name} growth w0=0")
        lines.append(f"R{name}n q{name} {b} {gradient_sensor(-GRADIENT)!r}")
    for name, a, b in SYNAPSES:
        lines.append(f"X{name} {a} {b} synapse w0=0")
    lines.append(".options vntol=1e-9")
    lines.append(f".tran 0.1m {stop}")
    lines.append(".print tran " + " ".join(f"v({n})" for n in POSITIONS) + " " +
                 " ".join(f"v(m{name}) v(p{name})" +
                          (f" i(R{name}p) i(R{name}n)" if currents else "")
                          for name, _, _ in SENSORS))
    lines.append(".end")
    return "\n".join(lines) + "\n"


def main():
    stop = sys.argv[1] if len(sys.argv) > 1 else "0.05"
    name = sys.argv[2] if len(sys.argv) > 2 else "axon_grow"
    currents = len(sys.argv) > 3 and sys.argv[3] == "currents"
    path = os.path.join(HERE, "circuits", name + ".cir")
    with open(path, "w") as file:
        file.write(netlist(stop, currents))
    print(path)


if __name__ == "__main__":
    main()
