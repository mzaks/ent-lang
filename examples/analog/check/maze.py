#!/usr/bin/env python3
"""The ways a maze's RRAM cells keep (maze.py: a wave of spikes from the
food, two cells an edge, set and reset by which end fired last) are the
ways a breadth-first search gives:

  examples/analog/check/maze.py [stop] [maze=moving] [sigma=s] [seed=n] [reltol=r] [way=0] [parallel=1] [...]

writes the circuit (maze.py, with what else is given: k=, c=, ug=, r=,
gs=, split=), prepares it, runs it the way given (0 unless; reltol 1e-3
unless given; `parallel=1`: the model built with --parallel), and reads
the waves as they come (Pac-Man's maze gives gigabytes). Each cell's
state z is worked out from its probes (its current and the voltage
across it, by its own g0) the last time it carried enough to, before the
time looked at: the middle of the run, the food of its first half, where
the food moves (`a` and `b` in maze.py's mazes), and the end. Read as a
chip would read it: each cell's resistance at 0.2 V, the pair's lower one
the way.

Held to, at each time looked at:

  edges     each edge between tiles at different distances from the food
            read the way to the nearer one (said: how many not, and how
            many of those are not by their states either, and the least
            ratio of a pair's two resistances);
  Pac-Man   from every tile not food, going each step the way its
            surest edge says (the largest ratio among those that point
            away from it), the food is reached (said: from how many
            tiles the first step goes nearer, and the steps more than the
            shortest in all).

Without spread (sigma 0) every tile must reach the food; with it, what
comes out is said, not held.
"""

import math
import os
import re
import subprocess
import sys
import time
from collections import deque

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common  # noqa: E402

ANALOG = os.path.dirname(HERE)
sys.path.insert(0, ANALOG)
import maze  # noqa: E402


def distances(open_, food, wrap):
    """Each tile's steps to the nearest food (None where none reaches)."""
    dist = {p: None for p in open_}
    todo = deque(food)
    for f in food:
        dist[f] = 0
    while todo:
        c, r = todo.popleft()
        for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            m = (c + d[0], r + d[1])
            if wrap:
                m = (m[0] % wrap, m[1])
            if m in dist and dist[m] is None:
                dist[m] = dist[(c, r)] + 1
                todo.append(m)
    return dist


def read_r(z, g0):
    """A cell's resistance at 0.2 V in the state z (rram.va's current)."""
    return 0.2 / (1e-3 * math.exp(-z * 1.8 / g0) * math.sinh(0.2 / 0.45))


def states(folder, rows, cir, times):
    """Each edge's two cells' (z, resistance) at each of `times`, from the
    waves as they come."""
    open_, _ = maze.tiles(rows)
    edges = maze.edges(open_, maze.wrap_of(rows))
    n = len(open_)
    g0, rs = {}, {}
    for m in re.finditer(r"^X([cd]\S+) \S+ \S+ rram(?:_rs)? (.*)$", cir, re.M):
        g0[m.group(1)] = float(re.search(r"g0=(\S+)", m.group(2)).group(1))
        found = re.search(r"rs=(\S+)", m.group(2))
        rs[m.group(1)] = float(found.group(1)) if found else 0.0
    split = re.search(r"^Rc", cir, re.M) is not None
    # (Per cell: the columns of its voltage's two ends, of its current, the
    # resistance in series within it, and its g0. Split (maze.py split=1):
    # the cell's from its tile to the node before its resistor, the current
    # the resistor's; else the tiles', less the current through rs.)
    cells = []
    for j, (x, y) in enumerate(edges):
        e = f"{maze.name(x)}_{maze.name(y)}"
        cx, cy = 1 + open_.index(x), 1 + open_.index(y)
        if split:
            c = 1 + n + 4 * j
            cells.append((cx, c, c + 1, 0.0, g0["c" + e]))
            cells.append((cy, c + 2, c + 3, 0.0, g0["d" + e]))
        else:
            c = 1 + n + 2 * j
            cells.append((cx, cy, c, rs["c" + e], g0["c" + e]))
            cells.append((cy, cx, c + 1, rs["d" + e], g0["d" + e]))
    # (A cell no voltage ever reaches is as it started, z = 1: the two ends
    # of Pac-Man's tunnel fire as one, his maze and its food being the same
    # either side.)
    last = [(1.0, read_r(1.0, g)) for _, _, _, _, g in cells]
    out = {}
    pending = sorted(times)
    with open(os.path.join(folder, "waves.txt")) as file:
        for line in file:
            row = line.split()
            t = float(row[0])
            while pending and t > pending[0]:
                out[pending.pop(0)] = list(last)
            for k, (ca, cb, ci, r, g) in enumerate(cells):
                u = float(row[ca]) - float(row[cb]) - float(row[ci]) * r
                if abs(u) > 0.05:
                    i = abs(float(row[ci]))
                    if i > 0:
                        z = -g / 1.8 * math.log(i / (1e-3 * abs(math.sinh(u / 0.45))))
                        last[k] = (z, read_r(z, g))
    for at in pending:
        out[at] = list(last)
    return edges, out


def judge(rows, edges, cells, half):
    """What the cells say against the search, for the food of `half`."""
    open_, food = maze.tiles(rows, half)
    wrap = maze.wrap_of(rows)
    dist = distances(open_, food, wrap)
    wrong = by_state = 0
    ratios, wrong_edges = [], []
    ways = {p: [] for p in open_}
    for j, (x, y) in enumerate(edges):
        (zc, rc), (zd, rd) = cells[2 * j], cells[2 * j + 1]
        if rc < rd:
            ways[x].append((rd / rc, y))
        else:
            ways[y].append((rc / rd, x))
        if dist[x] == dist[y]:
            continue
        nearer_y = dist[y] < dist[x]
        ratios.append(max(rc, rd) / min(rc, rd))
        if nearer_y != (rc < rd):
            wrong += 1
            wrong_edges.append((x, y))
        by_state += nearer_y != (zc < zd)
    first = reached = extra = 0
    lost = []
    for p in open_:
        if p in food:
            continue
        if ways[p] and dist[max(ways[p])[1]] < dist[p]:
            first += 1
        here, steps, seen = p, 0, set()
        while here not in food and here not in seen and ways[here]:
            seen.add(here)
            here = max(ways[here])[1]
            steps += 1
        if here in food:
            reached += 1
            extra += steps - dist[p]
        else:
            lost.append(p)
    return dict(edges=len(edges), wrong=wrong, wrong_edges=wrong_edges, by_state=by_state, ratio=min(ratios),
                tiles=len(open_) - len(food), first=first, reached=reached, extra=extra,
                lost=lost)


def main():
    words = sys.argv[1:]
    settings = dict(w.split("=", 1) for w in words if "=" in w)
    stop = float(next((w for w in words if "=" not in w), "0.1"))
    which = settings.pop("maze", "moving")
    reltol = settings.pop("reltol", "0.001")
    way = settings.pop("way", "0")
    parallel = settings.pop("parallel", "0") == "1"
    rows = maze.pacman_rows() if which == "pacman" else maze.MAZES[which]
    name = f"maze_{which}_check"
    subprocess.run([sys.executable, os.path.join(ANALOG, "maze.py"), repr(stop), name,
                    f"maze={which}", *(f"{k}={v}" for k, v in settings.items())],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(ANALOG, "prepare.py"), name],
                   check=True, stdout=subprocess.DEVNULL)
    cir_path = os.path.join(ANALOG, "circuits", name + ".cir")
    with open(cir_path) as file:
        cir = file.read()
    os.remove(cir_path)
    program = common.build(("--parallel",) if parallel else ())
    folder = os.path.join(common.RUN, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(common.CIRCUITS, name, "circuit.net")) as file:
        net = file.read()
    with open(os.path.join(folder, "circuit.net"), "w") as file:
        file.write(net)
    with open(os.path.join(folder, "run.txt"), "w") as file:
        file.write(f"{way} 1 1 {reltol} 0.000001 1e-12 1000000 0.0 0.0 0\n")
    began = time.time()
    said = subprocess.run([program], capture_output=True, text=True, cwd=folder).stdout.strip()
    print(f"{said} ({time.time() - began:.1f} s)")
    moving = any(ch in "ab" for line in rows for ch in line)
    looks = [(0, stop / 2 - 1e-3), (1, stop)] if moving else [(None, stop)]
    edges, cells = states(folder, rows, cir, [at for _, at in looks])
    os.remove(os.path.join(folder, "waves.txt"))
    failed = 0
    for half, at in looks:
        if any(c is None for c in cells[at]):
            print(f"at {at:.3f} s: a cell has carried nothing to read it by")
            failed += 1
            continue
        j = judge(rows, edges, cells[at], half)
        print(f"at {at:.3f} s: {j['wrong']} of {j['edges']} edges read the wrong way "
              f"({j['by_state']} by their states), a pair's ratio at least {j['ratio']:.1f}; "
              f"from {j['first']} of {j['tiles']} tiles the first step goes nearer the food, "
              f"from {j['reached']} the food is reached, {j['extra']} steps more than the "
              f"shortest in all" + (f"; lost from {j['lost']}" if j["lost"] else ""))
        if j["wrong_edges"]:
            print("   read the wrong way: " + " ".join(f"{x}-{y}" for x, y in j["wrong_edges"]))
        if float(settings.get("sigma", 0)) == 0 and j["lost"]:
            failed += 1
    if len(looks) == 2:
        # (That the food moving rewrote them: the edges whose way the
        # search turns between the halves, and those the cells turn.)
        open_, _ = maze.tiles(rows)
        wrap = maze.wrap_of(rows)
        d0 = distances(open_, maze.tiles(rows, 0)[1], wrap)
        d1 = distances(open_, maze.tiles(rows, 1)[1], wrap)
        turned = cells_turned = 0
        for k, (x, y) in enumerate(edges):
            if d0[x] != d0[y] and d1[x] != d1[y] and (d0[y] < d0[x]) != (d1[y] < d1[x]):
                turned += 1
            a, b = cells[looks[0][1]], cells[looks[1][1]]
            if (a[2 * k][1] < a[2 * k + 1][1]) != (b[2 * k][1] < b[2 * k + 1][1]):
                cells_turned += 1
        print(f"the food moved: the search turns {turned} edges, the cells {cells_turned}")
    print(f"maze {which}: " + ("as the search" if not failed else "NOT as the search"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
