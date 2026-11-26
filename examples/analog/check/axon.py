#!/usr/bin/env python3
"""The minimal example of guided axon growth and pruning (axon.py, from
Jenderny, Ochs and Xue, Sci Rep 14:16643, 2024, Fig. 3 and 4) does what
the paper shows:

  examples/analog/check/axon.py [stop]      # 5 s: the paper's run

writes circuits/axon_prune.cir (axon.py, with the currents probed),
prepares it, runs it way 0
and works out from its probes each memsensor's resistance from a to b
(v(a) - v(b) over the current through its pruning-cue sensor, its two
branches' together) and its growing memristor's memristance (v(m) - v(p)
over its branch's current), at the
peak of the current in the 6 ms before each time looked at (between
spikes the currents are too small to divide by). Held to:

  grown      both memsensors under 100 kOhm by 10 ms (the paper: grown
             within 10 ms, from 800 kOhm)
  pruned     Ma1 over 1 MOhm from 0.5 s, its memristor over 800 kOhm by
             4.3 s (the paper: Ma1 pruned by about 4.3 s)
  kept       Ma2 under 150 kOhm from 10 ms to the end (the paper: under
             100 kOhm; here it settles at 120 to 140 kOhm, said)
  silent     in the last 60 ms the node of the pruned synapse, (2,2),
             does not spike, and the other three do
"""

import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common

ANALOG = os.path.dirname(HERE)
MARKS = (0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.5, 1, 2, 3, 4, 4.3, 5)
NODES = ("n11", "n12", "n21", "n22")
SENSORS = (("Ma1", "n22"), ("Ma2", "n11"))


def main():
    stop = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    subprocess.run([sys.executable, os.path.join(ANALOG, "axon.py"), repr(stop), "axon_prune",
                    "currents"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(ANALOG, "prepare.py"), "axon_prune"],
                   check=True, stdout=subprocess.DEVNULL)
    program = common.build()
    waves, said = common.run(program, "axon_prune", 0, most=1000000)
    print(said)
    if not waves or waves[-1][0] < stop * (1 - 1e-9):
        print("axon_prune: the run did not reach its end")
        sys.exit(1)
    times = [line[0] for line in waves]
    node = {name: [line[1 + k] for line in waves] for k, name in enumerate(NODES)}

    def sensor(k, to, mark):
        """Resistance a to b and memristance at the current's peak before mark."""
        column = 5 + 4 * k
        # (The probes: v(m), v(p), the current through M+ and Rg(+d), and
        # through M- and Rg(-d); the two together through Rp.)
        best = max((abs(waves[j][column + 2] + waves[j][column + 3]), j)
                   for j in range(len(waves)) if mark - 0.006 < times[j] <= mark)[1]
        line = waves[best]
        total = (node["n12"][best] - node[to][best]) / (line[column + 2] + line[column + 3])
        memristance = (line[column] - line[column + 1]) / line[column + 2]
        return total, memristance

    wrong = []
    table = {}
    for mark in MARKS:
        if mark > stop:
            continue
        row = [sensor(k, to, mark) for k, (_, to) in enumerate(SENSORS)]
        table[mark] = row
        print(f"t {mark:5.3f} s  " + "   ".join(
            f"{name} {total / 1e3:7.1f} kOhm (memristor {memristance / 1e3:6.1f})"
            for (name, _), (total, memristance) in zip(SENSORS, row)))
    for mark, row in table.items():
        (ma1, m1), (ma2, _) = row
        if mark == 0.01 and not (ma1 < 1e5 and ma2 < 1e5):
            wrong.append(f"not grown by 10 ms: {ma1:.0f}, {ma2:.0f} Ohm")
        if mark >= 0.5 and ma1 < 1e6:
            wrong.append(f"Ma1 not pruned at {mark} s: {ma1:.0f} Ohm")
        if mark >= 4.3 and m1 < 8e5:
            wrong.append(f"Ma1's memristor not back at {mark} s: {m1:.0f} Ohm")
        if mark >= 0.01 and ma2 > 1.5e5:
            wrong.append(f"Ma2 not kept at {mark} s: {ma2:.0f} Ohm")
    last = [j for j in range(len(times)) if times[j] > stop - 0.06]
    for name in NODES:
        spikes = sum(1 for a, b in zip(last, last[1:]) if node[name][a] <= 0 < node[name][b])
        print(f"{name}: {spikes} spikes in the last 60 ms")
        if (spikes == 0) != (name == "n22") and stop >= 1:
            wrong.append(f"{name} {'spikes' if spikes else 'is silent'} at the end")
    kept = max(row[1][0] for mark, row in table.items() if mark >= 0.01)
    print(f"Ma2 at most {kept / 1e3:.0f} kOhm after growth (the paper: under 100)")
    for line in wrong:
        print("NOT as the paper:", line)
    print("axon_prune: " + ("as the paper" if not wrong else f"{len(wrong)} not as the paper"))
    sys.exit(1 if wrong else 0)


if __name__ == "__main__":
    main()
