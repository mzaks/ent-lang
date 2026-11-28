#!/usr/bin/env python3
"""Pac-Man eats his maze's dots steered by the RRAM cells alone, the
circuit in the game's loop (game.ent, maze.py game=1, way 3):

  examples/analog/check/game.py [stop] [maze=small] [reltol=r] [sigma=s] [seed=n] [...]

writes the circuit (with what else is given: k=, c=, ug=, r=, gs=),
prepares it, writes "game.txt" (the tiles' nodes and sources, the edges'
cells, by their numbers in the prepared circuit) and runs the model by
way 3 (reltol 1e-4 unless given) without its waves, for at most `stop`
seconds of the circuit (2 unless given; 12 for Pac-Man's maze), a move
each period of the food's pulses. The moves are said as they come, each
against a breadth-first search to the dots then left:

  nearer    the move goes a step nearer the nearest dot
  aside     it goes to a tile no nearer
  stayed    no pair of cells at his tile said a way

Held to: every move goes to a neighbour, and every dot is eaten before
the stop. The moves are kept in the run's folder ("moves.txt": the
time, the tile's column and row, whether he ate, the dots left, the
ratio of the pair he went by).
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


def game_text(net, rows, start):
    """game.txt for the prepared circuit `net`."""
    open_, food = maze.tiles(rows)
    edges = maze.edges(open_, maze.wrap_of(rows))
    node, source, cell = {}, {}, {}
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
            device += 1
    numbers = [repr(maze.PERIOD), repr(maze.PERIOD), len(open_), len(edges), device,
               open_.index(start)]
    for p in open_:
        n = node[maze.name(p)]
        numbers += [n, source.get(n, -1), 1 if p in food else 0]
    for x, y in edges:
        a, b = node[maze.name(x)], node[maze.name(y)]
        numbers += [open_.index(x), open_.index(y), cell[(a, b)], cell[(b, a)]]
    return " ".join(str(n) for n in numbers) + "\n"


def main():
    words = sys.argv[1:]
    settings = dict(w.split("=", 1) for w in words if "=" in w)
    which = settings.pop("maze", "small")
    stop = float(next((w for w in words if "=" not in w), "12" if which == "pacman" else "2"))
    reltol = settings.pop("reltol", "0.0001")
    rows, start = maze.game_rows(which)
    name = f"maze_{which}_game"
    subprocess.run([sys.executable, os.path.join(ANALOG, "maze.py"), repr(stop), name,
                    f"maze={which}", "game=1", *(f"{k}={v}" for k, v in settings.items())],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(ANALOG, "prepare.py"), name],
                   check=True, stdout=subprocess.DEVNULL)
    os.remove(os.path.join(ANALOG, "circuits", name + ".cir"))
    program = common.build()
    folder = os.path.join(common.RUN, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(common.CIRCUITS, name, "circuit.net")) as file:
        net = file.read()
    for file_name, text in (("circuit.net", net), ("game.txt", game_text(net, rows, start)),
                            ("run.txt", f"3 1 0 {reltol} 0.000001 1e-12 1000000 0.0 0.0 0\n")):
        with open(os.path.join(folder, file_name), "w") as file:
            file.write(text)
    open_, food = maze.tiles(rows)
    wrap = maze.wrap_of(rows)
    food = set(food)
    dots = len(food)
    here = start
    nearer = aside = stayed = 0
    failed = False
    said = ""
    began = time.time()
    # (stdbuf: each move as it is said, not when the program's lines fill a block.)
    run = subprocess.Popen(["stdbuf", "-oL", program], stdout=subprocess.PIPE, text=True,
                           cwd=folder)
    with open(os.path.join(folder, "moves.txt"), "w") as moves:
        for line in run.stdout:
            word = line.split()
            if not word or word[0] != "move":
                said += line
                continue
            at, tile, ate, left, ratio = float(word[1]), open_[int(word[2])], word[3] == "1", \
                int(word[4]), float(word[5])
            dist = distances(open_, food, wrap)
            if tile == here:
                stayed += 1
            else:
                step = (abs(tile[0] - here[0]), abs(tile[1] - here[1]))
                if step not in ((1, 0), (0, 1)) and not (wrap and step == (wrap - 1, 0)):
                    print(f"at {at:.3f} s: from {here} to {tile}, no neighbour")
                    failed = True
                if dist[tile] is not None and dist[here] is not None and dist[tile] < dist[here]:
                    nearer += 1
                else:
                    aside += 1
                    print(f"at {at:.3f} s: from {here} to {tile}, no nearer the nearest dot "
                          f"({dist[here]} steps off), by a pair's ratio of {ratio:.1f}")
            if ate != (tile in food) or left != len(food) - ate:
                print(f"at {at:.3f} s: at {tile} ate {ate}, {left} left: the dots here say "
                      f"{tile in food}, {len(food) - (tile in food)}")
                failed = True
            food.discard(tile)
            here = tile
            moves.write(f"{at!r} {tile[0]} {tile[1]} {int(ate)} {left} {ratio!r}\n")
            moves.flush()
            count = nearer + aside + stayed
            if count % 25 == 0:
                print(f"at {at:.3f} s: {count} moves, {len(food)} of {dots} dots left "
                      f"({time.time() - began:.0f} s)", flush=True)
    run.wait()
    print(f"{said.strip()} ({time.time() - began:.1f} s)")
    print(f"{nearer + aside + stayed} moves: {nearer} nearer the nearest dot, {aside} aside, "
          f"{stayed} stayed; {dots - len(food)} of {dots} dots eaten")
    failed = failed or bool(food)
    print(f"game {which}: " + ("every dot eaten" if not failed else "NOT every dot eaten"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
