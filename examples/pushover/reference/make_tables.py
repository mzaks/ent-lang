#!/usr/bin/env python3
"""Writes ../pictures.ent: the tables that say where in the game's sheets
of pictures each picture is, taken from the game's own source
(src/graphicsn.cpp, src/ant.cpp in build/pushover/upstream, after
build.sh): which row of data/dominos.png is which domino in which
state, which row of data/ant.png is which picture of which animation
and how far below its place it is drawn, and where the domino the ant
carries is drawn with each of them.

    python3 examples/pushover/reference/make_tables.py
"""
import os
import re

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(os.path.dirname(here)))
game = os.path.join(root, "build", "pushover", "upstream", "src")
graphics = open(os.path.join(game, "graphicsn.cpp")).read()
ant = open(os.path.join(game, "ant.cpp")).read()

names = {
    "DO_ST_FALLING": 1, "DO_ST_UPRIGHT": 8, "DO_ST_EXPLODE": 16,
    "DO_ST_SPLIT": 23, "DO_ST_CRASH": 51, "DO_ST_NUM": 57,
}
names["CARRIED_DOMINO_START"] = names["DO_ST_NUM"]


def body(source, start):
    """What is between the braces of the array declared at `start`."""
    at = source.index("{", source.index("=", source.index(start)))
    depth = 0
    for end in range(at, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[at + 1:end]
    raise SystemExit("no end of " + start)


def plain(text):
    return re.sub(r"/\*.*?\*/", "", re.sub(r"//[^\n]*", "", text), flags=re.S)


def numbers(text):
    return [eval(part.strip(), {}, names) for part in plain(text).split(",") if part.strip()]


def groups(text):
    return re.findall(r"\{([^{}]*)\}", plain(text))


# The dominoes: the pictures of each kind lie one after another in the
# sheet, in the order the game's table names the states they are of.
PICTURES = 64
rows = [-1] * (22 * PICTURES)
row = 0
for kind, listed in enumerate(groups(body(graphics, "dominoImages[DominoNumber][135]"))):
    for picture in numbers(listed):
        if picture == -1:
            break
        rows[kind * PICTURES + picture] = row
        row += 1
domino_pictures = row

# The ant: so many pictures each animation has; they lie one after
# another in the sheet, but some animations are others' pictures again.
counts = numbers(body(ant, "numAntAnimationsImages[AntAnimNothing]"))
offsets = numbers(body(graphics, "antOffsets[]"))
frames = {}
row = 0
used = 0


def read(animation):
    global row, used
    frames[animation] = []
    for _ in range(counts[animation]):
        frames[animation].append((row, offsets[used]))
        row += 1
        used += 1
    if animation == 48:        # (two pictures after "falling" are not used)
        row += 2


for animation in range(0, 28):
    read(animation)
for animation in (28, 29):     # the two before, backwards
    frames[animation] = list(reversed(frames[animation - 2]))
for animation in range(30, 44):
    read(animation)
for animation in (44, 45):     # those four before
    frames[animation] = list(frames[animation - 4])
for animation in range(46, 50):
    read(animation)
frames[50] = [frames[49][-1]]  # the last of the one before
for animation in range(51, 66):
    read(animation)
assert used == len(offsets), (used, len(offsets))
ant_pictures = row

# The domino the ant carries, for the animations from "carry left" on.
carried = []
for listed in groups(body(graphics, "moveOffsets[22][16*3]")):
    values = numbers(listed)
    values += [0] * (48 - len(values))
    carried.append(values)
assert len(carried) == 22

out = []
out.append("""// Pushover
//
// Pushover is the legal property of its developers (Andreas Röver,
// Volker Grabsch and the others its AUTHORS file names). This is a port
// of it to ent-lang.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// Where in the game's sheets of pictures each picture is (graphicsn.cpp).
// Written by reference/make_tables.py from the game's source: not to be
// changed by hand.

// Which row of the sheet of dominoes (data/dominos.png: a picture every
// 60 rows, 58 high) is a domino of a kind as it stands in a state: at
// `kind * 64 + state`. The states as the dominoes have them, and from 57
// on seven ways a domino is carried in. -1: there is no such picture.
""")
out.append("const DOMINO_PICTURES = 64\n")
out.append("const CARRIED_START = 57\n")
out.append("table domino_rows: i16 = [\n")
for kind in range(22):
    line = ", ".join(str(v) for v in rows[kind * PICTURES:(kind + 1) * PICTURES])
    out.append("  " + line + ("," if kind < 21 else "") + "\n")
out.append("]\n\n")
out.append("""// The ant's pictures (data/ant.png: one every 75 rows): `ant_first` is
// where in `ant_frames` each animation's pictures start, and a row of
// `ant_frames` says which picture of the sheet it is and how far below
// the ant's place it is drawn.
""")
firsts = []
at = 0
for animation in range(66):
    firsts.append(at)
    at += len(frames[animation])
out.append("table ant_first: i32 = [\n  " + ", ".join(str(v) for v in firsts) + "\n]\n")
out.append("table ant_frames { row: i16, below: i16 } = [\n")
listed = [frame for animation in range(66) for frame in frames[animation]]
for i in range(0, len(listed), 6):
    part = ", ".join("{ row: %d, below: %d }" % frame for frame in listed[i:i + 6])
    out.append("  " + part + ("," if i + 6 < len(listed) else "") + "\n")
out.append("]\n\n")
out.append("""// The domino the ant carries: for each of the 22 animations from "carry
// left" on, and each of up to 16 pictures of it, how far from the ant
// the domino is drawn and which of the domino's pictures (a state, or
// 57 and more for a way it is carried in): at `(animation - 10) * 16 +
// picture`.
""")
out.append("table carried_at { x: i16, y: i16, picture: i16 } = [\n")
for a, values in enumerate(carried):
    for i in range(0, 48, 12):
        part = ", ".join("{ x: %d, y: %d, picture: %d }" % tuple(values[j:j + 3]) for j in range(i, i + 12, 3))
        last = a == 21 and i == 36
        out.append("  " + part + ("" if last else ",") + "\n")
out.append("]\n")
open(os.path.join(os.path.dirname(here), "pictures.ent"), "w").write("".join(out))
print(f"{domino_pictures} pictures of dominoes, {ant_pictures} rows of the ant's ({len(listed)} used)")
