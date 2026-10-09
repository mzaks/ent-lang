#!/usr/bin/env python3
"""Writes examples/doom/info.ent from the reference's C.

  examples/doom/reference/make_info.py

Reads info.c, info.h, sounds.h and p_mobj.h of the Chocolate Doom that
fetch.sh put in build/doom: the sprites' names, the states a thing goes
through and what each kind of thing is, as ent-lang tables with a const
for every name. What it writes is committed; this is how it was made.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SOURCE = os.path.join(ROOT, "build", "doom", "ref", "chocolate-doom", "src",
                      "doom")
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

MOBJ_FIELDS = [
    "doomednum", "spawnstate", "spawnhealth", "seestate", "seesound",
    "reactiontime", "attacksound", "painstate", "painchance", "painsound",
    "meleestate", "missilestate", "deathstate", "xdeathstate", "deathsound",
    "speed", "radius", "height", "mass", "damage", "activesound", "flags",
    "raisestate",
]


def read(name):
    with open(os.path.join(SOURCE, name)) as file:
        return file.read()


def strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def enums(text, known):
    """The names of every enum in C source, with their values, in order."""
    found = []
    for body in re.findall(r"typedef\s+enum\s*\{(.*?)\}", strip(text), re.S):
        value = 0
        for entry in body.split(","):
            entry = entry.strip()
            if not entry:
                continue
            if "=" in entry:
                name, written = (part.strip() for part in entry.split("="))
                value = eval(written, {}, known)
            else:
                name = entry
            known[name] = value
            found.append((name, value))
            value += 1
    return found


def main():
    known = {"FRACUNIT": 1 << 16}
    info_names = enums(read("info.h"), known)
    enums(read("sounds.h"), known)
    flags = enums(read("p_mobj.h"), known)
    info = strip(read("info.c"))

    sprites = re.findall(r'"(\w{4})"', re.search(
        r"sprnames\[\]\s*=\s*\{(.*?)\}", info, re.S).group(1))

    actions = ["none"]
    states = []
    body = re.search(r"states\[NUMSTATES\]\s*=\s*\{(.*?)\n\};", info,
                     re.S).group(1)
    for sprite, frame, tics, action, after, misc1, misc2 in re.findall(
            r"\{(\w+),(\d+),(-?\d+),\{(\w+)\},(\w+),(-?\d+),(-?\d+)\}", body):
        if action == "NULL":
            action = "none"
        if action not in actions:
            actions.append(action)
        states.append((known[sprite], int(frame), int(tics),
                       actions.index(action), known[after], int(misc1),
                       int(misc2)))
    assert len(states) == known["NUMSTATES"], len(states)

    kinds = []
    body = re.search(r"mobjinfo\[NUMMOBJTYPES\]\s*=\s*\{(.*)\n\};", info,
                     re.S).group(1)
    for entry in re.findall(r"\{(.*?)\}", body, re.S):
        values = [eval(value.strip(), {}, known)
                  for value in entry.split(",") if value.strip()]
        assert len(values) == len(MOBJ_FIELDS), entry
        kinds.append(values)
    assert len(kinds) == known["NUMMOBJTYPES"], len(kinds)

    with open(os.path.join(DOOM, "info.ent"), "w") as file:
        file.write(LICENCE)
        file.write("""\
//
// What things are (info.c, info.h), written by reference/make_info.py:
// the sprites by their names, the states a thing goes through, and what
// each kind of thing starts as. Every sprite (SPR_..), state (S_..) and
// kind (MT_..) is a const, its number; so is every flag of a thing
// (MF_..) and everything a state may do (A_..).
import fixed

""")
        file.write("// A thing's flags (p_mobj.h).\n")
        for name, value in flags:
            if name.startswith("MF_"):
                file.write(f"const {name} = {value:#x}\n")
        file.write("""
// In a state's frame: drawn as light as can be, whatever the light; the
// rest of it says which picture.
const FF_FULLBRIGHT = 0x8000
const FF_FRAMEMASK = 0x7fff

// The sprites: four letters, the first the lowest byte of the number.
""")
        for name, value in info_names:
            if name.startswith("SPR_"):
                file.write(f"const {name} = {value}\n")
        file.write("table sprnames: u32 = [\n")
        for start in range(0, len(sprites), 6):
            row = ", ".join(
                f"{int.from_bytes(name.encode(), 'little'):#x}"
                for name in sprites[start:start + 6])
            file.write(f"  {row},  // {' '.join(sprites[start:start + 6])}\n")
        file.write("]\n\n// What a state may do when a thing comes to it.\n")
        for number, action in enumerate(actions):
            file.write(f"const {action if number else 'A_None'} = {number}\n")
        file.write("\n// The states.\n")
        for name, value in info_names:
            if name.startswith("S_"):
                file.write(f"const {name} = {value}\n")
        file.write("""
// A state: the sprite and its picture, how many tics it lasts (-1: for
// good), what it does, the state after it, and two numbers for what it
// does.
table states {
  sprite: i32, frame: i32, tics: i32, action: i32, nextstate: i32,
  misc1: i32, misc2: i32
} = [
""")
        rows = []
        for state in states:
            rows.append("  { sprite: %d, frame: %d, tics: %d, action: %d, "
                        "nextstate: %d, misc1: %d, misc2: %d }" % state)
        file.write(",\n".join(rows))
        file.write("\n]\n\n// The kinds of things.\n")
        for name, value in info_names:
            if name.startswith("MT_"):
                file.write(f"const {name} = {value}\n")
        file.write("""
// A kind of thing: the number the levels call it by (-1: none), its
// states, sounds and numbers, and the flags it starts with.
table mobjinfo {
""")
        file.write("  " + ", ".join(f"{field}: i32"
                                    for field in MOBJ_FIELDS) + "\n} = [\n")
        rows = []
        for values in kinds:
            rows.append("  { " + ", ".join(
                f"{field}: {value}"
                for field, value in zip(MOBJ_FIELDS, values)) + " }")
        file.write(",\n".join(rows))
        file.write("\n]\n")


if __name__ == "__main__":
    main()
