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
- `demo.ent`, `specials.ent`, `items.ent`, `play.ent`: a demo's keys tic
  by tic; the lights of sectors; what the player has and picks up; how
  the player is turned and made faster by the keys, and what of moving a
  thing needs no other thing: the walls around a place, the lines and
  things a path crosses.
- `enemy.ent`: what a thing does in its turn, by procs that are given
  the things (`things: mut Mobj`): it moves and falls, stopped by walls
  and things, the player sliding along them; it goes through its states
  and does what they do. Monsters look for the player and hear it, walk
  after it, shoot, bite, claw and throw fire; a shot's way through the
  level, what it leaves and what it does to what it hits; missiles and
  blasts; pain, death, and what the dead leave; teleporters.
- `weapon.ent`: the player's weapon in front of the eye (fist, saw,
  pistol, shotgun, chaingun, rocket launcher), the noise a shot makes,
  the floors that hurt, and the player dead.
- `movers.ent`: what the lines that are used, walked over or shot set
  off, and what then moves a sector: doors, lifts, floors, stairs,
  ceilings that crush, the donut; the ways out; lights switched.
- `tick.ent`: a tic of the level: the things, the lights and what moves
  sectors, each in its turn in the order they were made in (`for .. in
  turn by m.id`), the player's thing among them; a level left for the
  next, or started again after a death.
- `screen.ent`: the player's weapon over the view, and the status bar
  with its numbers and its face.
- `sounds.ent`, `audio.ent`: the sounds of a tic, each with how loud it
  is heard and from which side; and what plays them from the WAD's
  lumps, eight at a time (the sound device's `voice`). The music is not
  played: the WAD has it as notes for a synthesizer.
- Pictures that move, walls that slide and switches that show they were
  used are in `specials.ent`; the palettes the screen is shown in (red
  when hurt, yellow for something picked up, green in the suit) and the
  one colour map of seeing in the dark are chosen in `items.ent`.
- Not yet: the messages at the top of the screen, the border of a
  smaller view, the screens between levels (and the tics they take),
  the title and the menus; the monsters and weapons that are not in the
  first episode. Ceilings that move and the donut are written but were
  never set off: the shareware levels have no such ceiling, and no demo
  here uses the donut. The sounds were never heard by anyone yet.
- `doom.ent`: the program, which plays E1M1 with the keyboard (the arrow
  keys, the space bar to use, the left Ctrl key to shoot; Tab shows the
  map).
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
examples/doom/reference/fetch_demos.sh               # 53 more demos, of all nine levels
examples/doom/check/demos.sh                         # each of them, tic by tic, and its sounds
DUMP=205 examples/doom/check/demos.sh e1m5_uv_max    # what differs at a tic
examples/doom/check/shots.sh 3 0,50,100,300,600      # whole screens, and their palettes
examples/doom/check/shots.sh e1m5_uv_max 1700,3700   # of a fetched demo
cd build/doom/wad &&                                 # where doom1.wad is
  ../../../tools/ent run -I ../../../examples/doom ../../../examples/doom/doom.ent
```

The port follows the source of Doom as id Software released it and as
Chocolate Doom keeps it, so the files in this directory are under the GNU
General Public License, version 2 or later, as theirs are.
