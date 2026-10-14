#!/usr/bin/env python3
"""The waves are ngspice's (the spec's check 3).

  examples/analog/prepare.py all
  examples/analog/check/spice.py [circuits]

For each prepared circuit (all of build/analog/circuits, or those named)
ngspice runs the deck prepare.py wrote for it, with its tolerances made
tight (reltol 1e-6, vntol 1e-12, abstol 1e-15; its steps its own, by its
error, and breakpoints at the sources' corners), so that it stands for
the circuit's own waves. (It is not held to small steps: with 256
memristors it takes 20 ms a step, and a crossbar would take hours.)

The model runs the circuit at a fiftieth of its step, but no more than
200,000 time points, where backward Euler's error is small enough: at a
twentieth the ladder's first rise, 1 us, is 1.2e-3 off; the 16 by 16
crossbar of Yakopcic's devices, written, is 2.3e-3 of a range off at
50,000 time points, 1.2e-3 at 100,000 and 5.7e-4 at 200,000 (first
order, where its devices switch). Way 0 runs every circuit; ways 1 and 2
those of up to 100 nodes but crossbars with resistance in their wires
(check 4 holds them against way 0 on those, where they take thousands
of rounds a time point), with reltol 1e-5 and up to a million rounds a
time point. (At SPICE's reltol of 1e-3 a time point of relaxation is
solved to 1e-3 of its values, and a memristor's state carries what is
off on: the 4 by 4 crossbar with 10 ohm wires, written, comes out 2e-3
of the range off ngspice by way 1, which check 4 holds to way 0 within
its tolerance.)

ngspice's values are taken at the model's time points (in a straight
line between its own); every probe at every time point is to be within 1e-3 of the
probe's range over the run (its peak to peak in ngspice's waves; at an
operating point, which is one time point, its size; and 1e-12 at least).
Says the first that is not.

A memristor runs in ngspice as its model in models/<name>.va, which
OpenVAF (openvaf-r) compiles here into build/analog/models. Circuits too
big for a run of ngspice in minutes (above 600 nodes) are left out here.
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
MODELS = os.path.join(common.ROOT, "build", "analog", "models")


def models():
    """Every model in examples/analog/models, compiled for ngspice."""
    os.makedirs(MODELS, exist_ok=True)
    folder = os.path.join(common.ANALOG, "models")
    for file in sorted(os.listdir(folder)):
        if file.endswith(".va"):
            subprocess.run(["openvaf-r", os.path.join(folder, file), "-o",
                            os.path.join(MODELS, file[:-3] + ".osdi")],
                           check=True, capture_output=True)


def spice(name, net):
    """ngspice's waves: a list of lines, the time (or nothing at the
    operating point) and then the probes."""
    folder = os.path.join(common.RUN, f"{name}_spice")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    with open(os.path.join(common.CIRCUITS, name, "circuit.cir")) as file:
        deck = file.read()
    options = ".options reltol=1e-6 vntol=1e-12 abstol=1e-15 chgtol=1e-18 itl4=100\n"
    deck = deck.replace("\n.control", "\n" + options + ".control", 1)
    for model in re.findall(r"^pre_osdi (\S+)", deck, re.M):
        shutil.copy(os.path.join(MODELS, model), folder)
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
    models()
    failed = checked = 0
    for name in names:
        net = common.Netlist(name)
        if net.nodes > 600:
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
        # (Relaxation on a crossbar with resistance in its wires takes
        # thousands to millions of rounds a time point at reltol 1e-5:
        # check 4 holds it against way 0.)
        wire = name.split("_")[2] if name.startswith("crossbar_") else ""
        slow = bool(wire) and float(wire) > 0
        for way in ((0, 1, 2) if net.nodes <= 100 and not slow else (0,)):
            # (No more than 200,000 time points.)
            step = max(net.tran[0] / SHARE, net.tran[1] / 200000) if net.tran else 0.0
            waves, said = common.run(program, name, way, step=step, most=1000000,
                                     reltol=0.001 if way == 0 else 1e-5)
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
    failed += pinched(program)
    return 1 if failed else 0


def pinched(program):
    """A memristor under a sine: its current is 0 where its voltage is (the
    loop is pinched), and the loop's area shrinks as the frequency rises.
    The area of one period's loop is that of its two lobes, each the
    current taken along the voltage over half a period."""
    names = ["memristor_1hz", "memristor_10hz", "memristor_100hz"]
    if not all(os.path.isdir(os.path.join(common.CIRCUITS, n)) for n in names):
        return 0
    areas, failed = [], 0
    for name in names:
        net = common.Netlist(name)
        waves, _ = common.run(program, name, 0)
        hertz = net.devices[0][4][2]
        # (The last whole period, in two halves.)
        period = 1 / hertz
        start = net.tran[1] - period
        lobes = [0.0, 0.0]
        crossings = unpinched = 0
        for a, b in zip(waves, waves[1:]):
            (t0, v0, i0), (t1, v1, i1) = a, b
            if t0 >= start:
                lobes[0 if (t0 - start) < period / 2 else 1] += (i0 + i1) / 2 * (v1 - v0)
            if v0 * v1 < 0:
                crossings += 1
                unpinched += 0 if i0 * i1 <= 0 else 1
        areas.append(abs(lobes[0]) + abs(lobes[1]))
        if unpinched:
            failed += 1
        print(f"{name}: the voltage crosses 0 {crossings} times, the current with it "
              f"{crossings - unpinched} times; the loop's area {areas[-1]:.3e} V A")
    if not areas[0] > areas[1] > areas[2]:
        failed += 1
        print("the loop's area does NOT shrink as the frequency rises")
    else:
        print("the loop's area shrinks as the frequency rises")
    return failed


if __name__ == "__main__":
    sys.exit(main())
