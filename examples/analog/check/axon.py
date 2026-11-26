#!/usr/bin/env python3
"""The circuits of guided axon growth and pruning (axon.py, from Jenderny,
Ochs and Xue, Sci Rep 14:16643, 2024) do what the paper shows:

  examples/analog/check/axon.py [stop]         # the minimal example, 5 s (Fig. 4)
  examples/analog/check/axon.py grid [stop]    # the 15 by 15 grid, 5 s (Fig. 5)

writes the circuit (axon.py, with the currents probed: circuits/
axon_prune.cir, axon_grid.cir), prepares it, runs it way 0, and works
out from its probes each growing memsensor's resistance from a to b
(v(a) - v(b) over the current through its pruning-cue sensor, its two
branches' together) and its growing memristor's memristance (v(m) - v(p)
over its branch's current), at the peak of the current in the 6 ms
before each time looked at (between spikes the currents are too small to
divide by). The waves are read as they come, a line at a time (the
grid's are gigabytes).

The minimal example is held to:

  grown      both memsensors under 100 kOhm by 10 ms (the paper: grown
             within 10 ms, from 800 kOhm)
  pruned     Ma1 over 1 MOhm from 0.5 s, its memristor over 800 kOhm by
             4.3 s (the paper: Ma1 pruned by about 4.3 s)
  kept       Ma2 under 150 kOhm from 10 ms to the end (the paper: under
             100 kOhm; here it settles at 120 to 140 kOhm, said)
  silent     in the last 60 ms the node of the pruned synapse, (2,2),
             does not spike, and the other three do

The grid, its paths as axon.py's GRID_PATHS (Fig. 5a: N1 to N1' and N2
to N2' to be kept, N1 to N2' and N2 to N1' to be pruned):

  grown      every memsensor of the paths under 100 kOhm by 0.25 s (the
             paper: growth done at about 0.22 s), but the two into the
             synapses not to be, whose pruning has begun by then
  kept       those of the paths to be kept under 150 kOhm from 0.25 s to
             the end (the paper: under 100 kOhm; the one into N1''s
             synapse hears a quarter of its cue, as the minimal example's
             Ma2, and settles at 110 to 120 kOhm, said)
  pruned     on each path to be pruned, the memsensor into the synapse
             not to be over 1 MOhm from 1 s, and its memristor over 800
             kOhm from 4.5 s (the paper: the high resistance state
             reached by 4.5 s for almost all pruned connections); how
             many of each path's are, said
  firing     both targets spike in the last 60 ms
"""

import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common

ANALOG = os.path.dirname(HERE)
sys.path.insert(0, ANALOG)
import axon  # noqa: E402

MARKS = (0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.15, 0.22, 0.25, 0.5, 1, 2, 3, 4, 4.3, 4.5, 5)


def read(folder, layout, marks, stop):
    """From the waves as they come: each probed memsensor's resistance and
    memristance at each mark (at the peak of its current in the 6 ms
    before), each probed node's spikes (upward crossings of 0 V) in the
    last 60 ms, and the last time."""
    columns = {p: 1 + k for k, p in enumerate(layout.probed_nodes)}
    first = 1 + len(layout.probed_nodes)
    sensors = {name: (a, b) for name, a, b, _ in layout.sensors if name in layout.probed}
    at = {name: first + 4 * k for k, name in enumerate(layout.probed)}
    best = {}                       # (mark, name) -> (|i|, resistance, memristance)
    spikes = {p: 0 for p in layout.probed_nodes}
    before = None
    last = None
    with open(os.path.join(folder, "waves.txt")) as file:
        for line in file:
            row = [float(word) for word in line.split()]
            t = row[0]
            last = t
            for mark in marks:
                if not (mark - 0.006 < t <= mark):
                    continue
                for name, (a, b) in sensors.items():
                    c = at[name]
                    total = row[c + 2] + row[c + 3]
                    if abs(total) > best.get((mark, name), (0.0,))[0] and row[c + 2] != 0:
                        best[(mark, name)] = (abs(total),
                                              (row[columns[a]] - row[columns[b]]) / total,
                                              (row[c] - row[c + 1]) / row[c + 2])
            if before is not None and t > stop - 0.06:
                for p, k in columns.items():
                    if before[k] <= 0 < row[k]:
                        spikes[p] += 1
            before = row
    return best, spikes, last


def run(name, stop, flags):
    """The circuit written, prepared and run way 0 at reltol 1e-3; the
    folder of its waves."""
    subprocess.run([sys.executable, os.path.join(ANALOG, "axon.py"), repr(stop), name,
                    "currents", *flags], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(ANALOG, "prepare.py"), name],
                   check=True, stdout=subprocess.DEVNULL)
    program = common.build()
    folder = os.path.join(common.RUN, f"{name}_check")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(common.CIRCUITS, name, "circuit.net")) as file:
        net = file.read()
    with open(os.path.join(folder, "circuit.net"), "w") as file:
        file.write(net)
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write("0 1 1 0.001 0.000001 1e-12 1000000 0.0 0.0 0\n")
    said = subprocess.run([program], capture_output=True, text=True, cwd=folder).stdout.strip()
    print(said)
    return folder


def minimal(stop):
    layout = axon.minimal()
    marks = [m for m in MARKS if m <= stop]
    folder = run("axon_prune", stop, [])
    best, spikes, last = read(folder, layout, marks, stop)
    if last is None or last < stop * (1 - 1e-9):
        print("axon_prune: the run did not reach its end")
        return 1
    for mark in marks:
        print(f"t {mark:5.3f} s  " + "   ".join(
            f"M{name} {best[(mark, name)][1] / 1e3:7.1f} kOhm "
            f"(memristor {best[(mark, name)][2] / 1e3:6.1f})" for name in layout.probed))
    wrong = []
    for mark in marks:
        ma1, m1 = best[(mark, "a1")][1:]
        ma2 = best[(mark, "a2")][1]
        if mark == 0.01 and not (ma1 < 1e5 and ma2 < 1e5):
            wrong.append(f"not grown by 10 ms: {ma1:.0f}, {ma2:.0f} Ohm")
        if mark >= 0.5 and ma1 < 1e6:
            wrong.append(f"Ma1 not pruned at {mark} s: {ma1:.0f} Ohm")
        if mark >= 4.3 and m1 < 8e5:
            wrong.append(f"Ma1's memristor not back at {mark} s: {m1:.0f} Ohm")
        if mark >= 0.01 and ma2 > 1.5e5:
            wrong.append(f"Ma2 not kept at {mark} s: {ma2:.0f} Ohm")
    for p in layout.probed_nodes:
        name = layout.naming(p)
        print(f"{name}: {spikes[p]} spikes in the last 60 ms")
        if (spikes[p] == 0) != (p == (2, 2)) and stop >= 1:
            wrong.append(f"{name} {'spikes' if spikes[p] else 'is silent'} at the end")
    kept = max(best[(m, "a2")][1] for m in marks if m >= 0.01)
    print(f"Ma2 at most {kept / 1e3:.0f} kOhm after growth (the paper: under 100)")
    return verdict("axon_prune", wrong)


def grid(stop):
    layout = axon.grid()
    marks = [m for m in MARKS if m <= stop]
    folder = run("axon_grid", stop, ["grid"])
    best, spikes, last = read(folder, layout, marks, stop)
    if last is None or last < stop * (1 - 1e-9):
        print("axon_grid: the run did not reach its end")
        return 1
    named = {frozenset((a, b)): name for name, a, b, _ in layout.sensors}
    paths = [[named[frozenset(e)] for e in zip(path, path[1:])] for path in axon.GRID_PATHS]
    labels = ["N1 to N1' (kept)", "N1 to N2' (pruned)", "N2 to N2' (kept)",
              "N2 to N1' (pruned)"]
    kept, pruned = [0, 2], [1, 3]
    wrong = []
    for label, path in zip(labels, paths):
        print(label + ", each memsensor from the start, kOhm:")
        for mark in marks:
            print(f"  t {mark:5.3f} s " + " ".join(
                f"{best[(mark, name)][1] / 1e3:5.0f}" if (mark, name) in best else "    -"
                for name in path))
    ends = {paths[k][-1] for k in pruned}
    if stop >= 0.25:
        for path in paths:
            for name in path:
                r = best[(0.25, name)][1]
                if r > 1e5 and name not in ends:
                    wrong.append(f"{name} not grown by 0.25 s: {r:.0f} Ohm")
    for k in kept:
        for name in paths[k]:
            far = [m for m in marks if m >= 0.25 and best[(m, name)][1] > 1.5e5]
            if far:
                wrong.append(f"{name}, to be kept, at {best[(far[0], name)][1]:.0f} Ohm at "
                             f"{far[0]} s")
    most = max(best[(m, name)][1] for k in kept for name in paths[k] for m in marks if m >= 0.25)
    print(f"the paths to be kept at most {most / 1e3:.0f} kOhm after growth (the paper: under 100)")
    for k in pruned:
        end = paths[k][-1]
        for mark in marks:
            if mark >= 1 and best[(mark, end)][1] < 1e6:
                wrong.append(f"{end}, into the synapse not to be, at "
                             f"{best[(mark, end)][1]:.0f} Ohm at {mark} s")
            if mark >= 4.5 and best[(mark, end)][2] < 8e5:
                wrong.append(f"{end}'s memristor at {best[(mark, end)][2]:.0f} Ohm at {mark} s")
        back = [name for name in paths[k] if best[(marks[-1], name)][2] > 8e5]
        print(f"{labels[k]}: {len(back)} of its {len(paths[k])} memristors back over 800 kOhm "
              f"at {marks[-1]} s")
    for (target, _), label in zip(axon.GRID_TARGETS, ("N1'", "N2'")):
        print(f"{label}: {spikes[target]} spikes in the last 60 ms")
        if spikes[target] == 0 and stop >= 1:
            wrong.append(f"{label} silent at the end")
    return verdict("axon_grid", wrong)


def verdict(name, wrong):
    for line in wrong:
        print("NOT as the paper:", line)
    print(f"{name}: " + ("as the paper" if not wrong else f"{len(wrong)} not as the paper"))
    return 1 if wrong else 0


def main():
    words = sys.argv[1:]
    on_grid = "grid" in words
    numbers = [float(w) for w in words if w != "grid"]
    stop = numbers[0] if numbers else 5.0
    sys.exit(grid(stop) if on_grid else minimal(stop))


if __name__ == "__main__":
    main()
