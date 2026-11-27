#!/usr/bin/env python3
"""The three ways give the same waves, within the tolerances a time
point is solved to (the spec's check 4).

  examples/analog/check/agree.py [circuits] [most=N]

For each prepared circuit (all of build/analog/circuits but the slowest
crossbars, or those named)
the model runs three ways, the operating point and every time point
after: 0 direct, 1 relaxation by Gauss–Jacobi, 2 relaxation by events.
Every probe of ways 1 and 2 at every time point is to be way 0's (in a
straight line between way 0's time points, since each way chooses its
own steps) to within ten times the tolerance a node settles to
(reltol of the value and vntol, 1e-3 and 1e-6; for a current reltol of
the value and 1e-9 A), where the way settled (a point off on a jump,
a switching device's runaway, is held to when the jump comes:
common.jumps); a way that did not settle in
so many rounds (1,000,000) is said, and a circuit with a voltage source
between two nodes that are not ground is way 0's only. Says for each way
how many rounds and node steps it took, and how far off it is, in those
tolerances.

A circuit whose waves turn on when a switch comes (TIGHTER) is run all
three ways at a tighter reltol, and held to the same 1e-2: at 1e-3 the
steps each way chooses put the RRAM cell's set of rram_rs 1.4e-2 of its
current's range apart (3.1e-5 at 1e-4, 1.7e-9 at 1e-5). axon_grow's
spikes are held by spice.py, ways 1 and 2 to ngspice's spike times, not
here: way 0's at 1e-3 is the one off (2.3201 ms, at 1e-4 2.2943, way 1's
at 1e-3 2.2945), and at 1e-4 the ways' spikes are within a few us, but
on their edges, in millivolts, ten times 1e-4 of the value is less, and
ways 1 and 2 take 20 to 35 million rounds.
"""

import bisect
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common
import exact


# (The circuits run at a tighter reltol, and it; those ways 1 and 2 are not
# run for, spice.py holding them.)
TIGHTER = {"rram_rs": 1e-4}
BY_SPICE = {"axon_grow"}


def slow(name):
    """A crossbar left out unless named: by relaxation hours long (of 64 by
    64 and up, or of 16 by 16 with resistance in its wires whose states
    move: written, or of Yakopcic's, at 20,000 time points), or not
    settling in a million rounds at a pulse's edge (wires under 1 ohm)); and
    the IBM power grids, check 7's; and check/axon.py's runs of 5 s of
    spikes (axon_prune, axon_grid and their RRAM ones: minutes to a
    quarter of an hour by way 0, gigabytes of waves, and ways 1 and 2 do
    not do them); and check/maze.py's (their neurons too, and Pac-Man's
    maze nine minutes)."""
    if name.startswith(("ibmpg", "axon_prune", "axon_grid", "maze_")):
        return True
    if not name.startswith("crossbar_"):
        return False
    size, wire, model, scenario = name.split("_")[1:5]
    return int(size) >= 64 or 0 < float(wire) < 1 or (
        int(size) >= 16 and float(wire) > 0 and (scenario == "write" or model == "yakopcic"))


def main():
    arguments = sys.argv[1:]
    most = next((int(a[5:]) for a in arguments if a.startswith("most=")), 1000000)
    names = [a for a in arguments if not a.startswith("most=")] or \
        [n for n in sorted(os.listdir(common.CIRCUITS)) if not slow(n)]
    program = common.build()
    failed = 0
    for name in names:
        net = common.Netlist(name)
        t = exact.when(name)
        reltol = TIGHTER.get(name, 1e-3)
        base, said0 = common.run(program, name, 0, t, most, reltol=reltol)
        if not base:
            failed += 1
            print(f"{name}: way 0 gave nothing ({said0})")
            continue
        line = [f"{name}: " + (f"at reltol {reltol:g}, " if reltol != 1e-3 else "") +
                f"way 0 {said0.split(': ', 1)[-1]}"]
        for way in (1, 2):
            if name in BY_SPICE:
                line.append(f"way {way} held by spice.py")
                continue
            waves, said = common.run(program, name, way, t, most, reltol=reltol)
            if "does not do" in said:
                line.append(f"way {way} not for it")
                continue
            if not waves or "settled true" not in said:
                failed += 1
                line.append(f"way {way} NOT settled ({said})")
                continue
            worst, at = 0.0, None
            times = [w[0] for w in base]
            # (A point on a jump is held to when the jump comes:
            # common.jumps.)
            span = times[-1] - times[0] if len(times) > 1 else 0.0
            steps = [common.jumps(base, k, max(max(w[1 + k] for w in base) -
                                                min(w[1 + k] for w in base), 1e-300))
                     for k in range(len(net.probes))] if span > 0 else [[] for _ in net.probes]
            jumped = 0
            for got in waves:
                # (Way 0's waves at this way's time point, in a straight
                # line between its own: the steps each way chooses may
                # differ.)
                j = min(max(bisect.bisect_left(times, got[0]), 1), len(times) - 1) \
                    if len(times) > 1 else 0
                if len(times) > 1:
                    t0, t1 = times[j - 1], times[j]
                    share = 0.0 if t1 == t0 else (got[0] - t0) / (t1 - t0)
                    wanted = [got[0]] + [a + (b - a) * share
                                         for a, b in zip(base[j - 1][1:], base[j][1:])]
                else:
                    wanted = base[0]
                for k, ((kind, _), a, b) in enumerate(zip(net.probes, got[1:], wanted[1:])):
                    floor = 1e-6 if kind == "v" else 1e-9
                    share = abs(a - b) / (10 * (1e-3 * abs(b) + floor))
                    if share > 1 and common.on_a_jump(got[0], k, a, 10 * (1e-3 * abs(b) + floor),
                                                      base, steps[k], waves):
                        jumped += 1
                        continue
                    if share > worst:
                        worst, at = share, (k, a, b, got[0])
            rounds = said.split(" in ", 1)[-1]
            if worst > 1:
                failed += 1
                k, a, b, when = at
                line.append(f"way {way} NOT the same: probe {k} at {when} s {a!r}, way 0 {b!r} "
                            f"({rounds})")
            else:
                line.append(f"way {way} {rounds}, off by {worst:.3f}" +
                            (f" ({jumped} points on jumps, at their times)" if jumped else ""))
        print("; ".join(line))
    print(f"{len(names)} circuits, {failed} where the ways do not agree")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
