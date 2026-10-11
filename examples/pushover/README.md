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

Not here yet: anything seen or heard (the level drawn, the sounds and
the music), the menus, the choice of a level, and the second version of
the level files (all of the game's own levels are of the first).

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

## Licence

This is a port of Pushover's source and so, like it, under the GNU
General Public License, version 2 or later. Nothing of the game itself
is in this repository: its levels, pictures and sounds are read from
where `reference/build.sh` puts them. (The original levels and some of
the pictures are those of Ocean's game.)
