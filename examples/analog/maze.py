#!/usr/bin/env python3
"""A maze's ways to its food, found by a wave of spikes and kept in RRAM
cells (step A of the Pac-Man plan: the host's breadth-first search done
by a circuit).

Every open tile is a Morris-Lecar oscillator (axon.py's, Table 1 of
Jenderny, Ochs, Xue 2024, its voltages SCALE times, as axon.py's `rram`).
The food's tiles are fed pulses, and their spikes go on from tile to
tile through open ones (no wall has a device): between two neighbours a
synapse each way (the paper's, which opens when its first end is above
its second), so the tiles fire in the order of their distance to the
nearest food.

Each edge also has two of IHP's RRAM cells (models/rram.va), each in
series with a resistor: one from its first tile x to its second y, one
from y to x. When x fires, y not yet, the voltage across the first is up
and sets it; when y fires, it is down and resets it. So a cell ends in
the state of whichever end fired last: the one from x to y set where x
fired after y, so that from x the way to the food goes to y; the other
the other way. The pair is read against each other (the lower
resistance says the way), as RRAM arrays read a differential pair: a
cell's own spread moves its resistance as much as set and reset do, so
no one reference reads them all. The wave comes again each period of
the pulses and writes them anew, as the food moves.

  examples/analog/maze.py [stop] [name] [maze=chain] [r=...] [gs=...] [k=...] [c=...] [ug=...] [sigma=s] [seed=n] [game=1] [fear=1]

writes circuits/<name>.cir (maze_chain unless named): the mazes are
MAZES' below and `pacman`, examples/pacman.ent's (300 tiles, 320 edges
with the tunnel); `r` the cells' series resistance (500 Ohm), `gs` the
synapses' opened conductance (Gs1 of the paper's, 20 uS), `k` the
neurons' voltages over the paper's (20), `c` their conductances and
capacitance over the paper's (30; their times as they are), `ug` the
cells' gate voltage (0.8 V). Each as it is so, tried on the chain and the
5 by 5 mazes (check/maze.py):

  k 20      at 10 (axon.py's) a cell gets 0.6 V across it, on its set,
            and does not switch; at 15 the reset is not sure
  c 30      at 1 a set cell (12 kOhm) holds its two tiles together, they
            fire as one, and nothing is left across it to reset it; at
            10 not every cell resets (a real chip has transistors
            between: here the neurons are made stiffer than the cells)
  ug 0.8    at 1 V the cells set and do not reset where a tile fires
            soon after its neighbour (driven from two sides); under it a
            cell sets less far and resets faster (v0 / zeta^((ug-1)/0.2))
  r 500     a cell takes the voltage between its tiles, not its resistor

With `game=1` the maze is a game's (game.ent, check/game.py): a dot on
every tile of Pac-Man's maze that has one (on every tile but the first
of another maze), each fed its pulse every period until it is eaten.
With `fear=1` too the maze is there a second time, neurons, synapses
and cells of its own (nodes f<col>_<row>) and a source at every tile:
the layer game.ent sends the ghosts' wave through.

The resistor is the cell's own (models/rram_rs.va), so each cell is one
device between two tiles, its voltage and state solved within it; with
`split=1` a resistor of its own and a node between (the same circuit,
solved as nodes and states of the whole).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import axon  # noqa: E402

MAZES = {
    "chain": ["o.....",],
    "back": [".....o",],
    "corner": ["o..",
               "#.#",
               "#..",],
    # (`a` food the first half of the run, `b` the second.)
    "moving": ["a.#..",
               ".##..",
               "...#b",
               ".#...",
               "...#a",],
    "small": ["o.#..",
              ".##..",
              "...#.",
              ".#...",
              "...#o",],
}
# examples/pacman.ent's maze: `#` a wall, `-` the ghosts' door, `o` the
# power pellets, `.` the dots; read by pacman_rows() into one of these.
PACMAN = [
    "############################",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#o####.#####.##.#####.####o#",
    "#.####.#####.##.#####.####.#",
    "#..........................#",
    "#.####.##.########.##.####.#",
    "#.####.##.########.##.####.#",
    "#......##....##....##......#",
    "######.##### ## #####.######",
    "     #.##### ## #####.#     ",
    "     #.##          ##.#     ",
    "     #.## ###--### ##.#     ",
    "######.## #      # ##.######",
    "      .   #      #   .      ",
    "######.## #      # ##.######",
    "     #.## ######## ##.#     ",
    "     #.##          ##.#     ",
    "     #.## ######## ##.#     ",
    "######.## ######## ##.######",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#.####.#####.##.#####.####.#",
    "#o..##.......  .......##..o#",
    "###.##.##.########.##.##.###",
    "###.##.##.########.##.##.###",
    "#......##....##....##......#",
    "#.##########.##.##########.#",
    "#.##########.##.##########.#",
    "#..........................#",
    "############################",
]


def pacman_rows():
    """Pac-Man's maze as a maze here: the tiles he can reach from where he
    starts (13, 23), not through the ghosts' door; the top power pellets
    food the first half of the run (`a`), the bottom ones the second
    (`b`); every other tile he reaches open (`.`), the rest wall."""
    reach = {(13, 23)}
    todo = [(13, 23)]
    while todo:
        c, r = todo.pop()
        for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = ((c + d[0]) % 28, r + d[1])
            if 0 <= n[1] < 31 and n not in reach and PACMAN[n[1]][n[0]] not in "#-":
                reach.add(n)
                todo.append(n)
    rows = []
    for r, line in enumerate(PACMAN):
        out = ""
        for c, ch in enumerate(line):
            if (c, r) not in reach:
                out += "#"
            elif ch == "o":
                out += "a" if r < 15 else "b"
            else:
                out += "."
        rows.append(out)
    return rows


# The food's pulses: one wave a period.
PERIOD = 20e-3


def game_rows(which):
    """A maze for a game (game.ent), and where Pac-Man starts: his maze
    with a dot's or a power pellet's tile food (`o`) and he at (13, 23);
    another maze with every tile food but its first, where he starts."""
    if which == "pacman":
        rows = ["".join("#" if ch == "#" else "o" if PACMAN[r][c] in ".o" else "."
                        for c, ch in enumerate(line)) for r, line in enumerate(pacman_rows())]
        return rows, (13, 23)
    open_, _ = tiles(MAZES[which])
    rows = ["".join("#" if ch == "#" else "." if (c, r) == open_[0] else "o"
                    for c, ch in enumerate(line)) for r, line in enumerate(MAZES[which])]
    return rows, open_[0]


def tiles(rows, half=None):
    """The open tiles (col, row), and the food's (with `half` 0 or 1, that
    of that half of the run: `o` always, `a` the first, `b` the second;
    with none, every tile that is ever food)."""
    open_, food = [], []
    wanted = {None: "oab", 0: "oa", 1: "ob"}[half]
    for r, line in enumerate(rows):
        for c, ch in enumerate(line):
            if ch != "#":
                open_.append((c, r))
            if ch in wanted:
                food.append((c, r))
    return open_, food


def feeding(ch, stop, k, c):
    """The food's current: a pulse each PERIOD, over the run (`o`) or a
    half of it (`a`, `b`), as a PWL."""
    first, last = {"o": (0.0, stop), "a": (0.0, stop / 2), "b": (stop / 2, stop)}[ch]
    top = 2.4e-6 * k * c
    points = []
    start = first + 1e-3
    while start + 2.002e-3 < last:
        points += [(start, 0.0), (start + 1e-6, top), (start + 2.001e-3, top), (start + 2.002e-3, 0.0)]
        start += PERIOD
    if not points:
        return "0"
    return "PWL(" + " ".join(f"{t!r} {v!r}" for t, v in points) + ")"


def edges(open_, wrap=0):
    """Each pair of neighbours once: (x, y), x before y; with `wrap` (the
    maze's width), off one side is on at the other, as Pac-Man's tunnel."""
    here = set(open_)
    out = []
    for (c, r) in open_:
        for d in ((1, 0), (0, 1)):
            n = (c + d[0], r + d[1])
            if wrap and n[0] == wrap:
                n = (0, n[1])
            if n in here:
                out.append(((c, r), n))
    return out


def wrap_of(rows):
    """The maze's width where it wraps (Pac-Man's), else 0."""
    return len(rows[0]) if rows[0] == "#" * len(rows[0]) and len(rows[0]) == 28 else 0


def name(p):
    return f"t{p[0]}_{p[1]}"


def fear_name(p):
    return f"f{p[0]}_{p[1]}"


def power_tiles(which):
    """A game's power pellets: Pac-Man's four; another maze's last tile."""
    if which == "pacman":
        return [(1, 3), (26, 3), (1, 23), (26, 23)]
    return [tiles(MAZES[which])[0][-1]]


def netlist(rows, stop, r, gs, cells, title, k=axon.SCALE, c=1.0, ug=1.0, split=False,
            pulsed=False, fear=False):
    open_, food = tiles(rows)
    neuron = (f"ml gl={axon.GL * c!r} el={axon.EL * k!r} gna={axon.GNA * c!r} ena={axon.ENA * k!r} "
              f"una1={axon.UNA1 * k!r} una2={axon.UNA2 * k!r} gk={axon.GK * c!r} ek={axon.EK * k!r} "
              f"uk1={axon.UK1 * k!r} uk2={axon.UK2 * k!r}")
    synapse = (f"synapse gs1={gs * c!r} gs0={5e-6 * c!r} us1={40e-3 * k!r} us2={1e-3 * k!r}")
    lines = [title]
    for p in open_:
        lines.append(f"C{name(p)} {name(p)} 0 {2e-9 * c!r}")
        lines.append(f"X{name(p)} {name(p)} 0 {neuron} w0={axon.opened(axon.REST)!r}")
    for n, p in enumerate(food):
        # (A game's food: the same pulses as a source of its own kind, on
        # until game.ent turns it off.)
        lines.append(f"I{n + 1} 0 {name(p)} " +
                     (f"PULSE(0 {2.4e-6 * k * c!r} 1e-3 1e-6 1e-6 2e-3 {PERIOD!r})" if pulsed
                      else feeding(rows[p[1]][p[0]], float(stop), k, c)))
    probes = []
    for x, y in edges(open_, wrap_of(rows)):
        e = f"{name(x)}_{name(y)}"
        lines.append(f"Xs{e} {name(x)} {name(y)} {synapse} w0=0")
        lines.append(f"Xr{e} {name(y)} {name(x)} {synapse} w0=0")
        if split:
            lines.append(f"Xc{e} {name(x)} m{e} " +
                         cells.memristor().replace(" w0=", f" ug={ug!r} w0="))
            lines.append(f"Rc{e} m{e} {name(y)} {r!r}")
            lines.append(f"Xd{e} {name(y)} n{e} " +
                         cells.memristor().replace(" w0=", f" ug={ug!r} w0="))
            lines.append(f"Rd{e} n{e} {name(x)} {r!r}")
            probes.append(f"v(m{e}) i(Rc{e}) v(n{e}) i(Rd{e})")
        else:
            # (The resistor within the cell, rs: one device, its state
            # solved within it.)
            cell = cells.memristor().replace("rram ", "rram_rs ", 1).replace(
                " w0=", f" ug={ug!r} rs={r!r} w0=")
            lines.append(f"Xc{e} {name(x)} {name(y)} {cell}")
            lines.append(f"Xd{e} {name(y)} {name(x)} {cell}")
            probes.append(f"i(Xc{e}) i(Xd{e})")
    if fear:
        # (The maze a second time, for the wave from the ghosts: a source
        # at every tile, which game.ent turns on where a ghost is.)
        pulse = f"PULSE(0 {2.4e-6 * k * c!r} 1e-3 1e-6 1e-6 2e-3 {PERIOD!r})"
        for n, p in enumerate(open_):
            lines.append(f"C{fear_name(p)} {fear_name(p)} 0 {2e-9 * c!r}")
            lines.append(f"X{fear_name(p)} {fear_name(p)} 0 {neuron} w0={axon.opened(axon.REST)!r}")
            lines.append(f"If{n + 1} 0 {fear_name(p)} {pulse}")
        for x, y in edges(open_, wrap_of(rows)):
            e = f"{fear_name(x)}_{fear_name(y)}"
            cell = cells.memristor().replace("rram ", "rram_rs ", 1).replace(
                " w0=", f" ug={ug!r} rs={r!r} w0=")
            lines.append(f"Xs{e} {fear_name(x)} {fear_name(y)} {synapse} w0=0")
            lines.append(f"Xr{e} {fear_name(y)} {fear_name(x)} {synapse} w0=0")
            lines.append(f"Xc{e} {fear_name(x)} {fear_name(y)} {cell}")
            lines.append(f"Xd{e} {fear_name(y)} {fear_name(x)} {cell}")
    lines.append(".ic " + " ".join(f"v({name(p)})={axon.REST * k!r}" for p in open_) +
                 ("".join(f" v({fear_name(p)})={axon.REST * k!r}" for p in open_) if fear else ""))
    lines.append(".options vntol=1e-9")
    lines.append(f".tran 0.1m {stop}")
    lines.append(".print tran " + " ".join(f"v({name(p)})" for p in open_) + " " +
                 " ".join(probes))
    lines.append(".end")
    return "\n".join(lines) + "\n"


def main():
    words = sys.argv[1:]
    settings = dict(w.split("=", 1) for w in words if "=" in w)
    rest = [w for w in words if "=" not in w]
    stop = rest[0] if rest else "0.05"
    maze = settings.get("maze", "chain")
    title_name = rest[1] if len(rest) > 1 else f"maze_{maze}"
    cells = axon.Cells(True, 0.0, float(settings.get("sigma", 0)), int(settings.get("seed", 1)))
    game = settings.get("game", "0") == "1"
    rows = game_rows(maze)[0] if game else pacman_rows() if maze == "pacman" else MAZES[maze]
    text = netlist(rows, stop, float(settings.get("r", 500)),
                   float(settings.get("gs", 20e-6)), cells,
                   f"A maze's ways to its food by a wave of spikes, kept in RRAM cells: {maze}, {stop} s",
                   float(settings.get("k", 20)), float(settings.get("c", 30)),
                   float(settings.get("ug", 0.8)), settings.get("split", "0") == "1", game,
                   settings.get("fear", "0") == "1")
    path = os.path.join(HERE, "circuits", title_name + ".cir")
    with open(path, "w") as file:
        file.write(text)
    print(path)


if __name__ == "__main__":
    main()
