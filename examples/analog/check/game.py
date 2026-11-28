#!/usr/bin/env python3
"""Pac-Man eats his maze's dots steered by the RRAM cells alone, the
circuit in the game's loop (game.ent, maze.py game=1, way 3):

  examples/analog/check/game.py [stop] [maze=small] [ghosts=n] [pace=1] [fear=1] [reach=6] [fright=48] [reltol=r] [sigma=s] [seed=n] [...]

writes the circuit (with what else is given: k=, c=, ug=, r=, gs=),
prepares it, writes "game.txt" (the tiles' nodes and sources, the edges'
cells, by their numbers in the prepared circuit) and runs the model by
way 3 (reltol 1e-4 unless given) without its waves, for at most `stop`
seconds of the circuit (2 unless given; 12 for Pac-Man's maze), a move
each period of the food's pulses.

With `ghosts` (none unless given) the tiles at and next to a ghost are
held and not gone to, and the ghosts go a tile every `pace` periods, by
the arcade's rule (game.ent); in Pac-Man's maze they come in over their
house, the first at once, then after 8, 40 and 72 periods
(examples/pacman.ent's 1, 5 and 9 seconds), in another maze at its last
tiles at once. With them the circuit has the fear layer too (not with
`fear=0`): he is afraid where the ghosts' wave comes to his tile within
`reach` tiles' time (HOP seconds a tile after FIRST) and goes by that
layer then. A power pellet (Pac-Man's four; another maze's last tile)
frightens the ghosts for `fright` periods (0: no power pellets, dots);
one eaten is back after 24.

The moves are said as they come, each against a breadth-first search:

  nearer    the move goes a step nearer the nearest dot (the held tiles
            walls to the search)
  aside     it goes to a tile no nearer
  stayed    no pair of cells at his tile said a way
  cut off   the held tiles left no way to a dot: whatever he did
  fled      afraid, he went by the fear layer: said how many of these
            went further from the nearest ghost

Held to: every move goes to a neighbour and to no held tile; and,
without ghosts, every dot is eaten before the stop (with them it is
said how many, and whether he was caught). Said too: the time the
ghosts' wave took to his tile, by the tiles the nearest ghost was off.
The moves are kept in the run's folder ("moves.txt": the time, the
tile's column and row, whether he ate, the dots left, the ratio of the
pair he went by), and the game for replay.ent to show ("replay.txt":
its form is said there).
"""

import importlib.util
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common  # noqa: E402

ANALOG = os.path.dirname(HERE)
sys.path.insert(0, ANALOG)
import maze  # noqa: E402

# (check/maze.py's search, the check of the same name as the maze's file.)
_spec = importlib.util.spec_from_file_location("maze_check", os.path.join(HERE, "maze.py"))
_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_check)
distances = _check.distances


# The ghosts' wave: FIRST seconds from the pulse to a ghost's own tile's
# spike, HOP seconds a tile from there (measured: the run says them).
FIRST = 0.0
HOP = 0.15e-3


def game_text(net, rows, start, ghosts, pace, power, fear, fright):
    """game.txt for the prepared circuit `net`."""
    open_, food = maze.tiles(rows)
    edges = maze.edges(open_, maze.wrap_of(rows))
    node, source, cell, neuron, synapse = {}, {}, {}, {}, {}
    device = 0
    for line in net.splitlines():
        word = line.split()
        if not word:
            continue
        if word[0] == "node":
            node[word[2]] = int(word[1])
        elif word[0] in "RCVIM" and len(word[0]) == 1:
            if word[0] == "I":
                source[int(word[2])] = device
            elif word[0] == "M" and word[3] == "rram_rs":
                cell[(int(word[1]), int(word[2]))] = device
            elif word[0] == "M" and word[3] == "synapse":
                synapse[(int(word[1]), int(word[2]))] = device
            elif word[0] == "M" and word[3] == "ml":
                neuron[int(word[1])] = device
            device += 1
    layered = maze.fear_name(open_[0]) in node
    numbers = [repr(maze.PERIOD), repr(maze.PERIOD), len(open_), len(edges), device,
               open_.index(start), len(ghosts), pace, repr(fear if layered else 0.0), "1e-3",
               fright, 24, -0.4]
    for p in open_:
        n = node[maze.name(p)]
        f = node[maze.fear_name(p)] if layered else -1
        numbers += [n, source.get(n, -1), (2 if p in power else 1) if p in food else 0, p[0], p[1],
                    neuron[n], f, source[f] if layered else -1]
    for x, y in edges:
        a, b = node[maze.name(x)], node[maze.name(y)]
        numbers += [open_.index(x), open_.index(y), cell[(a, b)], cell[(b, a)],
                    synapse[(a, b)], synapse[(b, a)]]
        if layered:
            a, b = node[maze.fear_name(x)], node[maze.fear_name(y)]
            numbers += [cell[(a, b)], cell[(b, a)]]
        else:
            numbers += [-1, -1]
    for g, wait in ghosts:
        numbers += [open_.index(g), wait]
    return " ".join(str(n) for n in numbers) + "\n"


def letter(p, to, wrap):
    """The way from the tile p to its neighbour `to`, as replay.txt has it."""
    dc = to[0] - p[0]
    if wrap and abs(dc) == wrap - 1:
        dc = -dc
    return "r" if dc > 0 else "l" if dc < 0 else "d" if to[1] > p[1] else "u"


def main():
    words = sys.argv[1:]
    settings = dict(w.split("=", 1) for w in words if "=" in w)
    which = settings.pop("maze", "small")
    stop = float(next((w for w in words if "=" not in w), "12" if which == "pacman" else "2"))
    reltol = settings.pop("reltol", "0.0001")
    count = int(settings.pop("ghosts", "0"))
    pace = int(settings.pop("pace", "1"))
    layered = settings.pop("fear", "1") == "1" and count > 0
    reach = float(settings.pop("reach", "6"))
    fright = int(settings.pop("fright", "48"))
    rows, start = maze.game_rows(which)
    open_, food = maze.tiles(rows)
    wrap = maze.wrap_of(rows)
    power = set(maze.power_tiles(which)) if fright > 0 and count > 0 else set()
    waits = [0, 8, 40, 72]
    ghosts = [((13, 11), waits[k % 4]) if which == "pacman" else (open_[-1 - k], 0)
              for k in range(count)]
    name = f"maze_{which}_game"
    subprocess.run([sys.executable, os.path.join(ANALOG, "maze.py"), repr(stop), name,
                    f"maze={which}", "game=1", f"fear={int(layered)}",
                    *(f"{k}={v}" for k, v in settings.items())],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(ANALOG, "prepare.py"), name],
                   check=True, stdout=subprocess.DEVNULL)
    os.remove(os.path.join(ANALOG, "circuits", name + ".cir"))
    program = common.build()
    folder = os.path.join(common.RUN, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(common.CIRCUITS, name, "circuit.net")) as file:
        net = file.read()
    text = game_text(net, rows, start, ghosts, pace, power, FIRST + HOP * reach, fright)
    for file_name, text in (("circuit.net", net), ("game.txt", text),
                            ("run.txt", f"3 1 0 {reltol} 0.000001 1e-12 1000000 0.0 0.0 0\n")):
        with open(os.path.join(folder, file_name), "w") as file:
            file.write(text)
    food = set(food)
    dots = len(food)
    here = start
    nearer = aside = stayed = cut = fled = further = eaten = 0
    failed = caught = False
    said = ""
    ways = {p: "-" for p in open_}
    fears = {p: "-" for p in open_}
    # (Where each ghost is, and whether frightened, as of the wave he reads.)
    spooks = [[g if wait == 0 else None, False] for g, wait in ghosts]
    took = {}
    frames = []
    began = time.time()
    # (stdbuf: each move as it is said, not when the program's lines fill a block.)
    run = subprocess.Popen(["stdbuf", "-oL", program], stdout=subprocess.PIPE, text=True,
                           cwd=folder)
    with open(os.path.join(folder, "moves.txt"), "w") as moves:
        for line in run.stdout:
            word = line.split()
            if word and word[0] in ("says", "fear"):
                p = open_[int(word[1])]
                (ways if word[0] == "says" else fears)[p] = \
                    {"-1": "-", "-2": "x"}.get(word[2]) or letter(p, open_[int(word[2])], wrap)
                continue
            if word and word[0] == "ghost":
                g = open_[int(word[2])] if word[2] != "-1" else None
                spooks[int(word[1])] = [g, word[3] == "1"]
                frames[-1][0] += f" {g[0]} {g[1]} {word[3]}" if g else " -1 -1 0"
                continue
            if word and word[0] == "eaten":
                eaten += 1
                continue
            if word and word[0] == "caught":
                caught = True
                continue
            if not word or word[0] != "move":
                said += line
                continue
            at, tile, ate, left, ratio, afraid, fired = float(word[1]), open_[int(word[2])], \
                int(word[3]), int(word[4]), float(word[5]), word[6] == "1", float(word[7])
            # (The search: the tiles held for the wave he read are walls.)
            held = {p for p in open_ if ways[p] == "x"}
            dist = distances([p for p in open_ if p not in held], food - held, wrap)
            danger = [g for g, scared in spooks if g and not scared]
            off = distances(open_, danger, wrap) if danger else {}
            if fired >= 0 and off.get(here) is not None:
                took.setdefault(off[here], []).append(fired)
            if tile in held:
                print(f"at {at:.3f} s: from {here} to {tile}, a held tile")
                failed = True
            step = (abs(tile[0] - here[0]), abs(tile[1] - here[1]))
            if tile != here and step not in ((1, 0), (0, 1)) and \
                    not (wrap and step == (wrap - 1, 0)):
                print(f"at {at:.3f} s: from {here} to {tile}, no neighbour")
                failed = True
            if afraid:
                fled += 1
                further += off[tile] > off[here]
            elif dist.get(here) is None:
                cut += 1
            elif tile == here:
                stayed += 1
            elif dist.get(tile) is not None and dist[tile] < dist[here]:
                nearer += 1
            else:
                aside += 1
                print(f"at {at:.3f} s: from {here} to {tile}, no nearer the nearest dot "
                      f"({dist[here]} steps off), by a pair's ratio of {ratio:.1f}")
            if (ate > 0) != (tile in food) or left != len(food) - (ate > 0) or \
                    (ate == 2) != (tile in food and tile in power):
                print(f"at {at:.3f} s: at {tile} ate {ate}, {left} left: the dots here say "
                      f"{tile in food}, {len(food) - (tile in food)}")
                failed = True
            food.discard(tile)
            here = tile
            moves.write(f"{at!r} {tile[0]} {tile[1]} {ate} {left} {ratio!r}\n")
            moves.flush()
            frames.append([f"{at!r} {tile[0]} {tile[1]} {ate} {left} {ratio!r} {int(afraid)}"] +
                          ["".join(layer.get((c, r), " ") for r in range(len(rows))
                                   for c in range(len(rows[0]))) for layer in (ways, fears)])
            count = nearer + aside + stayed + cut + fled
            if count % 25 == 0:
                print(f"at {at:.3f} s: {count} moves, {len(food)} of {dots} dots left "
                      f"({time.time() - began:.0f} s)", flush=True)
    run.wait()
    with open(os.path.join(folder, "replay.txt"), "w") as file:
        file.write(f"{len(rows[0])} {len(rows)} {start[0]} {start[1]} {len(frames)} "
                   f"{maze.PERIOD!r} {len(ghosts)} {int(caught)}\n" +
                   "".join("".join("O" if (c, r) in power else ch for c, ch in enumerate(line)) + "\n"
                           for r, line in enumerate(rows)) +
                   " ".join(f"{g[0]} {g[1]} 0" if wait == 0 else "-1 -1 0" for g, wait in ghosts) +
                   "\n" + "".join(f"{a}\n{b}\n{c}\n" for a, b, c in frames))
    print(f"to see it: build examples/analog/replay.ent and start it in {folder}")
    print(f"{said.strip()} ({time.time() - began:.1f} s)")
    if took:
        print("the ghosts' wave to his tile, microseconds by the tiles off: " +
              ", ".join(f"{d}: {sum(took[d]) / len(took[d]):.0f}" for d in sorted(took)[:14]))
    print(f"{nearer + aside + stayed + cut + fled} moves: {nearer} nearer the nearest dot, "
          f"{aside} aside, {stayed} stayed, {cut} cut off, {fled} fled ({further} further from "
          f"the nearest ghost); {dots - len(food)} of {dots} dots eaten, {eaten} ghosts" +
          (", then caught" if caught else ""))
    failed = failed or (not ghosts and bool(food))
    print(f"game {which}: " + ("NOT as held to" if failed else "caught" if caught else
                               "every dot eaten" if not food else "dots left at the stop"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
