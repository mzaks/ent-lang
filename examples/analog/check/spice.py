#!/usr/bin/env python3
"""The waves are ngspice's (the spec's check 3).

  examples/analog/prepare.py all
  examples/analog/check/spice.py [circuits]

For each prepared circuit (all of build/analog/circuits, or those named)
ngspice runs the deck prepare.py wrote for it, with its tolerances made
tight (reltol 1e-6, vntol 1e-12, abstol 1e-15, and a time step no longer
than a fiftieth of the circuit's), so that it stands for the circuit's
own waves; and the model runs it all three ways, at a fiftieth of the
circuit's step, where backward Euler's error is small enough (at a
twentieth the ladder's first rise, 1 us, is 1.2e-3 off). ngspice's
values are taken at the model's time points (in a straight line between
its own); every probe at every time point is to be within 1e-3 of the
probe's range over the run (its peak to peak in ngspice's waves; at an
operating point, which is one time point, its size; and 1e-12 at least).
Says the first that is not.

Circuits with memristors wait for OpenVAF (milestone M3), and those too
big for a run of ngspice in a few seconds (crossbars of 16 by 16 and up)
are left out here.
"""

import bisect
import os
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common

SHARE = 50


def spice(name, net):
    """ngspice's waves: a list of lines, the time (or nothing at the
    operating point) and then the probes."""
    folder = os.path.join(common.RUN, f"{name}_spice")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    with open(os.path.join(common.CIRCUITS, name, "circuit.cir")) as file:
        deck = file.read()
    options = ".options reltol=1e-6 vntol=1e-12 abstol=1e-15 chgtol=1e-18 itl4=100\n"
    if net.tran:
        step, stop = net.tran[0], net.tran[1]
        deck = re.sub(r"^\.tran .*$", f".tran {step!r} {stop!r} 0 {step / SHARE!r}", deck,
                      flags=re.M)
    deck = deck.replace("\n.control", "\n" + options + ".control", 1)
    with open(os.path.join(folder, "circuit.cir"), "w") as file:
        file.write(deck)
    said = subprocess.run(["ngspice", "-b", "circuit.cir"], capture_output=True, text=True,
                          cwd=folder, timeout=600)
    path = os.path.join(folder, "spice.txt")
    if not os.path.exists(path):
        return None, said.stdout[-500:] + said.stderr[-500:]
    lines = []
    with open(path) as file:
        for line in file:
            words = line.split()
            try:
                lines.append([float(w) for w in words])
            except ValueError:
                continue
    return lines, ""


def main():
    names = sys.argv[1:] or sorted(os.listdir(common.CIRCUITS))
    program = common.build()
    failed = checked = 0
    for name in names:
        net = common.Netlist(name)
        if any(device[0] == "M" for device in net.devices):
            print(f"{name}: has memristors, which wait for OpenVAF (M3)")
            continue
        if net.nodes > 100:
            continue
        theirs, why = spice(name, net)
        if theirs is None:
            failed += 1
            print(f"{name}: ngspice gave nothing: {why}")
            continue
        checked += 1
        times = [line[0] for line in theirs]
        columns = len(theirs[0]) - 1

        def at(t, k):
            """ngspice's probe k at time t, in a straight line between."""
            if not net.tran:
                return theirs[0][1 + k]
            j = min(max(bisect.bisect_left(times, t), 1), len(times) - 1)
            t0, t1 = times[j - 1], times[j]
            v0, v1 = theirs[j - 1][1 + k], theirs[j][1 + k]
            return v0 if t1 == t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)

        ranges = [max(1e-12, max(line[1 + k] for line in theirs) -
                      min(line[1 + k] for line in theirs),
                      abs(theirs[0][1 + k]) if not net.tran else 0.0)
                  for k in range(columns)]
        report = []
        points = 0
        for way in (0, 1, 2):
            step = net.tran[0] / SHARE if net.tran else 0.0
            waves, said = common.run(program, name, way, step=step)
            if "does not do" in said:
                continue
            points = max(points, len(waves))
            if not waves or "settled true" not in said:
                failed += 1
                report.append(f"way {way} did not settle ({said})")
                continue
            worst, where = 0.0, None
            for line in waves:
                for k in range(columns):
                    share = abs(line[1 + k] - at(line[0], k)) / ranges[k]
                    if share > worst:
                        worst, where = share, (line[0], k, line[1 + k], at(line[0], k))
            if worst > 1e-3:
                failed += 1
                t, k, mine, theirs_ = where
                report.append(f"way {way} NOT ngspice's: probe {k} at {t} s is {mine!r}, "
                              f"ngspice {theirs_!r}")
            else:
                report.append(f"way {way} within {worst:.1e} of the range")
        print(f"{name}: {points} time points; " + "; ".join(report))
    print(f"{checked} circuits held against ngspice, {failed} not as it")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
