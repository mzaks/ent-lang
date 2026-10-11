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
  turn by m.id`), the player's thing among them; a level started again
  after a death, or left for the next through the tics in which what
  was done in it is counted up. `between.ent` draws those: the map of
  the episode, the numbers, and where it goes on.
- `screen.ent`: the player's weapon over the view, the status bar with
  its numbers and its face, what the player is told at the top, and
  what is around a view that is smaller than the screen.
- `melt.ent`: one kind of screen melts into another: a level into the
  screen after it, that into the next level, a demo into a picture.
- `sounds.ent`, `audio.ent`: the sounds of a tic, each with how loud it
  is heard and from which side; and what plays them from the WAD's
  lumps, eight at a time (the sound device's `voice`).
- `music.ent`, `chip.ent`: the music. The WAD has a tune as notes and
  says how the chip of a sound card of the time is to be set for each
  instrument. `music.ent` reads the notes as their time comes and turns
  them into what is written to that chip, as the game's own driver did:
  the same values at the same times as the reference's, for every tune.
  `chip.ent` is the chip: nine voices of two waves each, written from
  what its registers mean. It is a simple one that does not make the
  same samples as the reference's exact one, but its sound is near it
  in every measure tried. `audio.ent` hands its samples to the sound
  device as they are made (`stream`).
- Pictures that move, walls that slide and switches that show they were
  used are in `specials.ent`; the palettes the screen is shown in (red
  when hurt, yellow for something picked up, green in the suit) and the
  one colour map of seeing in the dark are chosen in `items.ent`.
- After the last level its text is written out, and then the picture
  shown (`tick_finale`, `draw_finale`).
- `shell.ent`: what is around the game: the title, the WAD's three
  demos played one after another with a picture between them, and the
  menus over any of it (m_menu.c): a new game in one of four skills;
  the options (ending the game, whether anything is said at the top,
  the view in half as many columns, the view's size, how loud the
  sounds and the music are); what to read; and leaving, which asks
  first. What is asked is answered with y or n.
- Not yet: saving and loading, the fifth skill, the mouse (its setting
  is in the menu and does nothing), making the picture lighter (F11);
  the monsters and weapons that are not in the first
  episode. Ceilings that move and the donut are written but were
  never set off: the shareware levels have no such ceiling, and no demo
  here uses the donut.
- `doom.ent`: the program. It starts with the title and the demos; any
  key brings the menu (the arrow keys, Enter, Backspace to go back, a
  line's first letter), and a new game plays the episode from E1M1 with
  the keyboard (the arrow keys, A and D to step aside, Shift to run, the
  space bar to use, the left Ctrl key to shoot, 1 to 7 for a weapon; Tab
  shows the map; - and = make the view smaller and bigger).
  Esc opens and shuts the menu at any time; the game stands still
  while it is open. The F keys are the game's: F1 what to read, F4 how
  loud, F5 the detail, F7 ends the game, F8 whether anything is said,
  F10 leaves.
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
examples/doom/reference/fetch_demos.sh               # 56 more demos: of all nine levels, and through the episode
examples/doom/check/demos.sh                         # each of them, tic by tic, and its sounds
DUMP=205 examples/doom/check/demos.sh e1m5_uv_max    # what differs at a tic
examples/doom/check/shots.sh 3 0,50,100,300,600      # whole screens, and their palettes
examples/doom/check/shots.sh e1m5_uv_max 1700,3700   # of a fetched demo
examples/doom/check/shots.sh ep1_nomo 300,330,400    # between two levels too
BLOCKS=6 examples/doom/check/shots.sh 3 0,50,100     # with a smaller view
DETAIL=1 examples/doom/check/shots.sh 3 0,50,100     # in half as many columns
MELTS=1 examples/doom/check/shots.sh ep1_nomo 100    # and every screen of the melts
examples/doom/check/music.sh                         # what each tune writes to the chip
SOUND=1 examples/doom/check/music.sh D_E1M1 60       # and how near the chip's sound is
cd build/doom/wad &&                                 # where doom1.wad is
  ../../../tools/ent run -I ../../../examples/doom ../../../examples/doom/doom.ent
```

The port follows the source of Doom as id Software released it and as
Chocolate Doom keeps it, so the files in this directory are under the GNU
General Public License, version 2 or later, as theirs are.
