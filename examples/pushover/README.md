# Pushover in ent-lang

A port of [Pushover](https://github.com/pushover/pushover.github.io) (its
tag 0.0.5), the free reimplementation of Ocean's game of 1992: an ant
rearranges dominoes on platforms so that one push topples them all, the
trigger last, and leaves by the door. It is to play the game's own
recordings as the game does, with the dominoes and the ant as entities
and what moves them as systems. It is being built; what is here so far:

- `level.ent`: a level, 20 places wide and 25 high. A domino is an
  entity that knows its place, and the buffer `Cells` says who stands
  where (`who: entity`), with the platform and the ladder of each place:
  so a domino finds its neighbour without a search. A level and a
  recording are read from the game's own files.
- `dominoes.ent`: what a domino does in a tick by its kind and how it
  stands, to itself and to its neighbours: it falls, pushes the next,
  leans, splits, goes off, bridges a gap, rises to the ceiling, goes over
  an edge and down, and becomes a pile with what it lands on. They take
  their turns place by place (`for e, d: mut Domino in turn by d.cell`),
  each finding done what those before it did. A domino that goes to
  another place is a new entity there and the old one is no more: where
  that place comes later in the tick it has a turn there too, as in the
  game, whose recordings depend on it.
- `ant.ent`: the ant, an entity too: what it does next by the keys and
  what is around it, animation by animation; and a tick of the whole
  level (`tick`: the doors, the ant, the dominoes, what it came to).

- `pictures.ent`, `draw.ent`: a level drawn. The game's pictures are
  sheets (every domino in every state, the ant's animations, a theme's
  blocks), and `picture_level` lists which part of which sheet goes
  where, in the order they are drawn in (`Blits`): the layers of the
  background, the doors and the platforms, the dominoes, the ladders and
  the ant with the domino it carries. `pictures.ent` is the game's own
  tables of where each picture is, written by `reference/make_tables.py`.
  A theme's list of blocks is read from its file.
- `sets.ent`: the sets of levels, each a folder of the game's `levels`
  and a list of its files (an `asset` with a `*`): a set and a level of
  one are entities, read when the program starts.
  Which levels were solved is kept in a file (`pushover.solved`, where
  the program is started).
- `menu.ent`: the windows over the game: the main menu, the choice of a
  set and of a level (with what is solved in its colour), what is set
  (the whole screen, the sounds, the music), what to do when a level is
  left, solved, solved too late or lost, the help (what is to be done
  or, once the time is up, the level's hint; the dominoes the level
  has), and who made the game. A window's lines are entities, made when
  it is opened.
- `pushover.ent`: the game in a window: the menus, a level played with
  the keys at 18 ticks a second, the sounds and each theme's music.

Not here yet: the players' profiles and the level editor (the main menu
has neither), recordings written of what is played, the pages of the
help where a level has more kinds of dominoes than fit, a set's authors
in its description, the light that the game lays over a background, and
the second version of the level files (all of the game's own levels are
of the first). The game's font is FreeSans, which it does not bring
along: a font of the machine is taken, or else the window's own. What
was solved is kept by a level's file, not by a sum over the level as
the game keeps it.

## Playing it

    examples/pushover/reference/build.sh        # fetches the game
    examples/pushover/reference/pictures.sh     # draws the dominoes
    examples/pushover/reference/assets.sh       # puts its files here
    ent run examples/pushover/pushover.ent

The game does not keep the pictures of its dominoes: they are drawn by
POV-Ray from a scene it has, when it is built. `pictures.sh` does that
(it says how to get POV-Ray without installing it for the machine).

The game's files are the program's assets: `assets.sh` copies them next
to the port's sources (`data`, `themes`, `levels`; not committed), and
the modules that use them declare them (`asset "themes/*.png"`, `asset
original = "levels/original/*.level"`). So the program is started from
anywhere, `ent build` writes them next to the program, and

    ent build --embed examples/pushover/pushover.ent -o pushover

puts them into it: one file of some 33 MB that needs nothing beside it
(but a font of the machine). The levels of a set are the files that are
there when the program is built.

In a menu the arrow keys choose and Enter takes; Escape goes back. In a
level the arrow keys walk and climb. The space bar takes the domino the
ant stands in front of, and puts it down again. With up held the ant
goes in among the dominoes, and there space with left or right pushes.
Escape asks what now (go on, start again, what is set, back to the
levels), F1 helps.

## Checked against the game

`reference/build.sh` fetches the game into `build/pushover` (not
committed) and builds of it a program that plays a recording with the
game's own logic and writes a line for each tick: the keys, what the
tick came to, the time left, all of the ant, the doors, the platforms
and ladders, every domino and the sounds the tick started.
`check/trace.sh` has the port play the same recording and write the
same lines, and compares them. All 244 recordings the game keeps (235 of
solved levels, 7 that fail, 2 that end in piles) are the same in every
tick.

    examples/pushover/reference/build.sh
    examples/pushover/check/trace.sh            # all of them
    examples/pushover/check/trace.sh 010_a      # one, saying what differs

The menus are gone through without a window by `check/menu.sh`: keys
from a list, a level played by its recording to its end, and what each
window holds written out.

What is drawn is checked by eye, and without a window:
`check/frame.sh finish/original/010_a 215 out.png` plays so many ticks
of a recording and puts the picture together of what `picture_level`
lists.

## Licence

This is a port of Pushover's source and so, like it, under the GNU
General Public License, version 2 or later. Nothing of the game itself
is in this repository: its levels, pictures and sounds are fetched by
`reference/build.sh` and put next to the port by `reference/assets.sh`.
A program built with `--embed` holds them, and is the game's to pass on
under that licence. (The original levels and some of
the pictures are those of Ocean's game.)
