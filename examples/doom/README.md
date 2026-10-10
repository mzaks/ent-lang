# Doom in ent-lang

A port of Doom (the shareware episode, v1.9) that is to play the original
demos as the original does, with its things as entities and what moves
them as systems. It is being built; what is here so far:

- `fixed.ent`, `tables.ent`, `angles.ent`, `random.ent`: fixed point, the
  angle tables and what computes with them, and the random numbers.
- `wad.ent`, `level.ent`: the WAD and its lumps, and a level read out of
  it: vertexes, sectors and sides in buffers, the lines as entities.
- `video.ent`, `automap.ent`: the picture of 320 by 200 pixels with its
  palette, and the map of the level drawn into it.
- `data.ent`: the textures, put together of their patches, the flats and
  the colour maps.
- `render.ent`: the view of the level: the walk through its tree from the
  front to the back, the pieces of walls that are seen, and the walls,
  floors, ceilings and sky drawn.
- `info.ent`, `things.ent`: the kinds of things with the states they go
  through, and the things of a level as entities.
- `sprites.ent`: what is drawn after the walls: the things by their
  sprites, and textures in openings.
- `demo.ent`, `specials.ent`, `items.ent`, `play.ent`, `movers.ent`: a
  demo's keys tic by tic; the lights of sectors; what the player has and
  picks up; a thing moved through the level, stopped by walls and things,
  the player sliding along them; and doors and lifts, set off by the use
  key, by lines that are walked over, and by monsters.
- `enemy.ent`: a tic of every thing, each in its turn in the order they
  were made in (`for .. in turn by m.id`), the player's among them; and
  of what monsters do, that they look for the player (the line of sight
  through the level's tree) and walk after it once they have seen it.
  Not yet: that anything attacks or is hurt (weapons, shots, damage,
  death), floors, ceilings and stairs that move, teleporters, the ways
  out, floors that hurt.
- `doom.ent`: the program, which so far shows E1M1 from where the player
  starts (the arrow keys move the eye; Tab shows the map).
- `check/`: programs that print what the port computes, to hold against
  the reference.
- `reference/`: what the port is compared against. `fetch.sh` fetches
  Chocolate Doom and the shareware WAD into `build/doom` and builds the
  first with `trace.patch`, which makes it write a line for each tic of a
  level; `compare.py` finds the first tic at which two such traces
  differ. `make_tables.py` wrote `tables.ent` and `random.ent` from the
  reference's C, `make_info.py` wrote `info.ent`.

```sh
examples/doom/reference/fetch.sh demo1 demo2 demo3   # the traces to match
examples/doom/check/maths.sh                         # "the same: 24 lines"
examples/doom/check/automap.sh                       # E1M1 read and drawn
examples/doom/check/view.sh                          # what is seen, pixel for pixel
examples/doom/check/trace.sh                         # the demos, tic by tic
cd build/doom/wad &&                                 # where doom1.wad is
  ../../../tools/ent run -I ../../../examples/doom ../../../examples/doom/doom.ent
```

The port follows the source of Doom as id Software released it and as
Chocolate Doom keeps it, so the files in this directory are under the GNU
General Public License, version 2 or later, as theirs are.
