#!/usr/bin/env python3
"""Puts a picture together of what check/frame_check.ent wrote: `frame.py
GAME OUT.png < lines`, where GAME is where the game is
(build/pushover/upstream). Each line is a part of a sheet (0 the
dominoes, 1 the ant, 2 the theme named in the first line) and where it
goes in a picture of 800 by 600, laid over what is there."""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
source = open(os.path.join(here, "..", "reference", "assemble.py")).read()
tools = {}
exec(compile(source.replace("\nmain()\n", "\n"), "assemble.py", "exec"), tools)

game, out = sys.argv[1], sys.argv[2]
lines = sys.stdin.read().splitlines()
theme = lines[0].split()[1]
sheets = {
    0: tools["read_png"](os.path.join(game, "data", "dominos.png")),
    1: tools["read_png"](os.path.join(game, "data", "ant.png")),
    2: tools["read_png"](os.path.join(game, "themes", theme + ".png")),
}
wide, high = 800, 600
picture = [bytearray(b"\0\0\0\xff" * wide) for _ in range(high)]
for line in lines[1:]:
    sheet, from_x, from_y, w, h, x, y = (int(part) for part in line.split())
    width, height, rows = sheets[sheet]
    for dy in range(h):
        if not (0 <= y + dy < high and 0 <= from_y + dy < height):
            continue
        row = rows[from_y + dy]
        to = picture[y + dy]
        for dx in range(w):
            if not (0 <= x + dx < wide and 0 <= from_x + dx < width):
                continue
            at = (from_x + dx) * 4
            alpha = row[at + 3]
            if alpha == 0:
                continue
            put = (x + dx) * 4
            for c in range(3):
                to[put + c] = (row[at + c] * alpha + to[put + c] * (255 - alpha)) // 255
tools["write_png"](out, wide, high, picture)
print(f"{out}: {len(lines) - 1} parts")
