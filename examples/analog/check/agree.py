#!/usr/bin/env python3
"""The three ways give the same operating point, within the tolerances
a time point is solved to (the spec's check 4).

  examples/analog/check/agree.py [circuits] [most=N]

For each prepared circuit (all of build/analog/circuits, or those named)
the model solves the operating point three ways: 0 direct, 1 relaxation
by Gauss–Jacobi, 2 relaxation by events. Every probe of ways 1 and 2 is
to be way 0's to within ten times the tolerance a node settles to
(reltol of the value and vntol, 1e-3 and 1e-6; for a current reltol of
the value and 1e-9 A), where the way settled; a way that did not settle in
so many rounds (100,000) is said, and a circuit with a voltage source
between two nodes that are not ground is way 0's only. Says for each way
how many rounds and node steps it took, and how far off it is, in those
tolerances. A crossbar's operating point is at 50 ms, the others' at 0.
"""

import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common
import exact


def main():
    arguments = sys.argv[1:]
    most = next((int(a[5:]) for a in arguments if a.startswith("most=")), 100000)
    names = [a for a in arguments if not a.startswith("most=")] or \
        sorted(os.listdir(common.CIRCUITS))
    program = common.build()
    failed = 0
    for name in names:
        net = common.Netlist(name)
        t = exact.when(name)
        base, said0 = common.run(program, name, 0, t, most)
        if not base:
            failed += 1
            print(f"{name}: way 0 gave nothing ({said0})")
            continue
        line = [f"{name}: way 0 {said0.split(': ', 1)[-1]}"]
        for way in (1, 2):
            waves, said = common.run(program, name, way, t, most)
            if "does not do" in said:
                line.append(f"way {way} not for it")
                continue
            if not waves or "settled true" not in said:
                failed += 1
                line.append(f"way {way} NOT settled ({said})")
                continue
            worst, at = 0.0, None
            for k, ((kind, _), a, b) in enumerate(zip(net.probes, waves[0][1:], base[0][1:])):
                floor = 1e-6 if kind == "v" else 1e-9
                share = abs(a - b) / (10 * (1e-3 * abs(b) + floor))
                if share > worst:
                    worst, at = share, (k, a, b)
            rounds = said.split(" in ", 1)[-1]
            if worst > 1:
                failed += 1
                k, a, b = at
                line.append(f"way {way} NOT the same: probe {k} {a!r}, way 0 {b!r} ({rounds})")
            else:
                line.append(f"way {way} {rounds}, off by {worst:.3f}")
        print("; ".join(line))
    print(f"{len(names)} circuits, {failed} where the ways do not agree")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
