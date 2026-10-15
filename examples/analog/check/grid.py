#!/usr/bin/env python3
"""The IBM power grids' operating points are their published solutions
(the spec's check 7).

  examples/analog/fetch.sh && examples/analog/prepare.py ibmpg1 ...
  examples/analog/check/grid.py [grids] [ways=0,1,2] [most=N]

For each IBM grid prepared (ibmpg1 to ibmpg6, those fetched; or those
named) way 0 solves the operating point, and every node is to be the
published solution's to within 1e-5 V: the solution has six digits, of
values up to 1.8 V, so it is itself up to 5e-6 V off what it
stands for, and the vias, sources of 0 V, are each made one node of two
by prepare.py (merged.txt names the nodes that went into another). Ways
1 and 2 (relaxation) say whether they settled, in how many rounds (up to
`most`, by default 100,000) and node steps, and how far off the solution
they came out. Way 0 failing fails the check; ways 1 and 2 not settling
does not (spec section 6.4: that is a result).
"""

import os
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

GRIDS = os.path.join(common.ROOT, "build", "analog", "ibmpg")
ALLOWED = 1e-5


def published(name):
    """The published solution: the voltage of each node, by name."""
    voltages = {}
    with open(os.path.join(GRIDS, name + ".solution")) as file:
        for line in file:
            words = line.split()
            if len(words) == 2:
                voltages[words[0].lower()] = float(words[1])
    return voltages


def nodes(name):
    """Each node's index in circuit.net, by name, the merged ones too."""
    # (The solutions name ground G.)
    index = {"g": 0, "0": 0}
    folder = os.path.join(common.CIRCUITS, name)
    with open(os.path.join(folder, "circuit.net")) as file:
        for line in file:
            if line.startswith("node "):
                _, k, node = line.split()
                index[node] = int(k)
    merged = os.path.join(folder, "merged.txt")
    if os.path.exists(merged):
        with open(merged) as file:
            for line in file:
                node, k = line.split()
                index[node] = int(k)
    return index


def compared(waves, net, index, wanted):
    """The largest difference from the solution, and where."""
    voltage = {0: 0.0}
    for (kind, which), value in zip(net.probes, waves[0][1:]):
        voltage[which] = value
    worst, at = 0.0, None
    for node, want in wanted.items():
        if node not in index:
            return None, f"no node {node} in the circuit"
        off = abs(voltage[index[node]] - want)
        if off > worst:
            worst, at = off, f"{node}: {voltage[index[node]]!r}, published {want!r}"
    return worst, at


def main():
    arguments = sys.argv[1:]
    most = next((int(a[5:]) for a in arguments if a.startswith("most=")), 100000)
    ways = next(([int(w) for w in a[5:].split(",")] for a in arguments
                 if a.startswith("ways=")), [0, 1, 2])
    names = [a for a in arguments if "=" not in a] or \
        [n for n in sorted(os.listdir(common.CIRCUITS)) if n.startswith("ibmpg")]
    if not names:
        print("no IBM grid prepared: examples/analog/fetch.sh, then prepare.py ibmpg1 ...")
        return 0
    program = common.build()
    failed = 0
    for name in names:
        if not os.path.exists(os.path.join(GRIDS, name + ".solution")):
            print(f"{name}: no published solution in {GRIDS}; left out")
            continue
        net = common.Netlist(name)
        index = nodes(name)
        wanted = published(name)
        for way in ways:
            began = time.time()
            waves, said = common.run(program, name, way, most=most)
            took = time.time() - began
            how = said.split(": ", 1)[-1] if said.startswith(f"way {way}") else said
            if not waves:
                print(f"{name}, way {way}: no operating point ({how}), {took:.1f} s")
                failed += way == 0
                continue
            worst, at = compared(waves, net, index, wanted)
            good = worst is not None and worst <= ALLOWED
            if way == 0 and not good:
                failed += 1
            print(f"{name}, way {way}: {how}; {took:.1f} s; " +
                  (f"{len(wanted)} nodes, the most off {worst:.2e} V ({at})"
                   if worst is not None else at) +
                  ("" if good or way != 0 else ": NOT the published solution"))
    print(f"{len(names)} grids, {failed} not as published")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
