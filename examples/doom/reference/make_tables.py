#!/usr/bin/env python3
"""Writes examples/doom/tables.ent and random.ent from the reference's C.

  examples/doom/reference/make_tables.py

Reads tables.c and m_random.c of the Chocolate Doom that fetch.sh put in
build/doom, and writes their tables as ent-lang tables. What it writes is
committed; this is how it was made.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SOURCE = os.path.join(ROOT, "build", "doom", "ref", "chocolate-doom", "src")
DOOM = os.path.dirname(HERE)

LICENCE = """\
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
"""


def numbers(text, name, count):
    """The values of the array `name` in C source."""
    match = re.search(re.escape(name) + r"\[\d+\]\s*=\s*\{(.*?)\};", text,
                      re.S)
    values = [int(word) for word in re.findall(r"-?\d+", match.group(1))]
    assert len(values) == count, (name, len(values))
    return values


def table(name, kind, values, each=8):
    lines = [f"table {name}: {kind} = ["]
    for start in range(0, len(values), each):
        row = ", ".join(str(value) for value in values[start:start + each])
        lines.append(f"  {row},")
    lines[-1] = lines[-1].rstrip(",")
    lines.append("]")
    return "\n".join(lines) + "\n"


def main():
    with open(os.path.join(SOURCE, "tables.c")) as file:
        tables = file.read()
    with open(os.path.join(SOURCE, "doom", "m_random.c")) as file:
        random = file.read()

    with open(os.path.join(DOOM, "tables.ent"), "w") as file:
        file.write(LICENCE)
        file.write("""\
//
// The lookup tables of angles (tables.c), written by
// reference/make_tables.py; what computes with them is in angles.ent.
//
// An angle is a u32, the whole turn its whole range: a quarter turn is
// 0x40000000, and a sum that goes past a whole turn starts again. A fine
// angle is the top 13 bits of one (`angle >> 19`), 8192 to the turn.

// The tangent of a fine angle from -90 to 90 degrees, in 16.16 fixed
// point: 4096 of them, the half turn.
""")
        file.write(table("finetangent", "i32",
                         numbers(tables, "finetangent", 4096)))
        file.write("""
// The sine of a fine angle, in 16.16 fixed point: a turn and a quarter,
// so that the cosine is the sine a quarter turn on (`finesine[a + 2048]`).
""")
        file.write(table("finesine", "i32", numbers(tables, "finesine", 10240)))
        file.write("""
// The angle whose tangent is n / 2048, for n from 0 to 2048: the first
// eighth of a turn.
""")
        file.write(table("tantoangle", "u32",
                         numbers(tables, "tantoangle", 2049)))

    with open(os.path.join(DOOM, "random.ent"), "w") as file:
        file.write(LICENCE)
        file.write("""\
//
// The random numbers (m_random.c): 256 of them in a fixed order, the
// table written by reference/make_tables.py. Every game that draws them
// the same number of times goes the same way, which is what lets a demo
// be played back.
//
// `Random.play` is where the game is in them (P_Random: all that decides
// what happens), `Random.other` where everything else is (M_Random: what
// is only seen or heard). To draw one, step on and read:
//
//   Random.play = random_after(Random.play)
//   let chance = random_at(Random.play)
unique Random { play: i32, other: i32 }

""")
        file.write(table("rndtable", "u8", numbers(random, "rndtable", 256),
                         each=14))
        file.write("""
// The place after `index`: round the table.
fn random_after(index: i32) -> i32 { (index + 1) & 0xff }

// The number at a place, 0 to 255.
fn random_at(index: i32) -> i32 { rndtable[index] as i32 }
""")


if __name__ == "__main__":
    main()
