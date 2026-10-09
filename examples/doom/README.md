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
  floors, ceilings and sky drawn. Not yet what is masked: the things in
  the level and the textures in openings.
- `doom.ent`: the program, which so far shows E1M1 from where the player
  starts (the arrow keys move the eye; Tab shows the map).
- `check/`: programs that print what the port computes, to hold against
  the reference.
- `reference/`: what the port is compared against. `fetch.sh` fetches
  Chocolate Doom and the shareware WAD into `build/doom` and builds the
  first with `trace.patch`, which makes it write a line for each tic of a
  level; `compare.py` finds the first tic at which two such traces
  differ. `make_tables.py` wrote `tables.ent` and `random.ent` from the
  reference's C.

```sh
examples/doom/reference/fetch.sh demo1 demo2 demo3   # the traces to match
examples/doom/check/maths.sh                         # "the same: 24 lines"
examples/doom/check/automap.sh                       # E1M1 read and drawn
examples/doom/check/view.sh                          # what is seen, pixel for pixel
cd build/doom/wad &&                                 # where doom1.wad is
  ../../../tools/ent run -I ../../../examples/doom ../../../examples/doom/doom.ent
```

The port follows the source of Doom as id Software released it and as
Chocolate Doom keeps it, so the files in this directory are under the GNU
General Public License, version 2 or later, as theirs are.
