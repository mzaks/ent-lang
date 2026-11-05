# 05 · Pac-Man, Doom, Pushover and a UI library

*6 to 16 October 2026. Games as a forcing function: write a real program,
see what the language lacks, add it properly, and check the result against
the original.*

## Pac-Man: "ok, this looks pretty cool"

On 6 October `examples/pacman.ent` landed: a maze, a mouth and four ghosts,
with no C of its own. The first time it ran in a window was the moment I
stopped thinking of ent-lang as a compiler experiment and started thinking
of it as a language. (I'm still a little surprised how much a yellow circle
with a missing slice can do for morale.)

The game is 702 lines, and its frame is a schedule anyone can read:

```
schedule frame() {
  begin()
  flow()
  player() run_if Game.state == State.Playing
  eat() run_if Game.state == State.Playing
  ghosts() run_if Game.state == State.Playing
  draw_maze()
  draw_pellets()
  draw_pac() run_if Game.state != State.Over
  play()
  present()
}
```

(Trimmed. The real one also has the states between levels.)

The language had to grow for it:

- `fn` with a body, which only computes and can return several values;
- `var`, tracked by the parser through `if`s and loops, so the IR gets
  nothing new;
- `enum`, stored as a byte and printed by name;
- a math module;
- sound as spawned `Tone` entities;
- the window's `sector`, for the mouth.

That became the pattern for everything below. **A game finds a hole, and
the hole gets a proper language feature, not a workaround in the game.**

Snake and a tower defence game followed on 8 and 9 October. Towers brought
`table`s (rows of constant data, one per enum case if you like), `prefab`s,
and a `for` inside a `for`, so that a shell landing can hurt every foe near
it.

## Trees, and a UI library people said wouldn't work

On 6 October relations learned to be **trees**, driven by examples: an
orrery first, then a river network as the benchmark. You could reach the
parent, walk the tree depth by depth, and add up into ancestors. At a million
nodes the generated river matched C written the same way. C that kept the
nodes in depth order took 0.4× the time. So sorting became something you ask
for (`tree sorted`), not something done behind your back, because "it comes
with its own pitfalls".

Then the syntax changed to Cypher-style arrows, at my request. The keywords
`up`/`before`/`down` were gone the same day:

```
for (child: FitW)-[Inside]->(e) { t.a += child.fit }
connect (side)-[Inside]->(outer)
```

The real test for trees was a UI. The common wisdom is that ECS and UI don't
mix: UIs are deep hierarchies with order, layout passes in both directions,
and things that change one at a time. So on 7 and 8 October we wrote a layout
library in ent-lang (`examples/layout.ent`, 990 lines, plus 1,212 for its
window companion) and held it against **Clay**, Nic Barker's C layout library,
feature by feature:

- sizing as fit, grow or fixed;
- padding, gaps and alignment;
- text wrapping;
- floats, clipping and scrolling with momentum;
- rounded corners, borders and pictures;
- hover;
- transitions with enter and leave;
- an inspector.

Three of Clay's own examples were ported: the video demo, transitions and the
sidebar.

A box is an entity, and its parent is an arrow:

```
let side = spawn {
  Box { x: 0.0, y: 0.0, w: 300.0, h: 0.0 },
  Size { w: Sizing.Fixed, h: Sizing.Grow },
  Stack { row: false, pad: 16.0, gap: 16.0 }, Fill { colour: 0x9696ff }
}
connect (side)-[Inside]->(outer)
```

Each layout pass is a `for` that goes `bottom up Inside` or `top down Inside`,
and reacts only to what changed (`on changed s.w, changed (: FitW)-[Inside]->(e)`).
Making that cheap was most of the work. With 90,301 boxes and one changing
height per frame:

| | µs a frame |
|---|---|
| every pass walks the whole tree | 1,664 |
| passes follow their events | 196 |
| and only new boxes are checked for setup | 18 |

That's 83 times less than walking everything. The language features that came
out of the UI work were used everywhere afterwards:

- `optional` bindings with `if let`;
- `loop ... until` with entity `for`s inside;
- a `for` that returns how many entities it ran for;
- `while`;
- fns and procs as values;
- `text` of any length;
- `changed` that fires only when a value really differs.

## Doom: the port Claude said wouldn't fit

Before we started, Claude told me Doom was not a good fit for an ECS
language. Fair point:

- Doom's thinkers run one after another in the order they were made, and
  each sees what the one before it did;
- damage is immediate;
- the level is a BSP tree walked front to back;
- everything is linked lists and fixed point.

I asked for it anyway, as "a very close port": **demo sync, but within
reason**. It had to stay ECS-native, with no C written in ent. Where Doom
doesn't fit, grow the language, and read the WAD at run time.

The port took three days, 9 to 11 October, and it grew the language as it
went:

- unsigned integers and bit operations;
- `const`s;
- buffers that procs can read;
- an entity's components handed to a proc (`things: mut Mobj`);
- and the one that made it ECS and not C in disguise: **a `for` whose
  entities take turns.**

```
for e, m: mut Mobj in turn by m.id where !m.gone && ... {
  ...
  thing_tic(Lines, Sectors, ..., Mobj, Spawned.player, Movers, m.id)
}
```

Each entity is a `for` of its own, in spawn order. It reads the world as the
ones before it left it, and what it writes is there for the next. Something
spawned during the loop gets its turn in the same `for`, as in Doom. It's
sequential, of course: such a `for` isn't fused or threaded. But it's
declared as such, so the compiler knows exactly where the parallel parts end.

The checks were never "looks right". They were byte-for-byte against
Chocolate Doom, patched to write a trace line per tic:

- **The view:** the pictures from 92 places in E1M1 against the reference's,
  "all are the same, pixel for pixel". Getting there meant building the
  reference with `-fwrapv`. A sum of angles has to wrap as it did in 1993,
  and the modern C compiler had other ideas.
- **The demos:** first the WAD's three, then 53 more fetched demos (one of
  them 56,411 tics long), then 56. Every one is the same as the reference
  in every tic to the end, sounds included. With `SKILL=4` the 56 demos
  replay at Nightmare, and port and reference still agree in every tic.
- **Whole screens**, between levels and through the screen melts.
- **The music:** the WAD stores tunes as notes for the FM chip of a 1993
  sound card. `music.ent` drives the chip as the original driver did, and
  `chip.ent` *is* the chip: nine voices of two waves each, written in
  ent-lang from what its registers mean. All thirteen tunes write the same
  values at the same times as the reference, over six minutes each. Our chip
  is simpler than the reference's exact emulator, but on seven tunes the
  spectra agree to 0.94–0.99.
- **Save games:** the language's `save_world`. A demo saved at tic 500, 1500
  or 3000 and loaded into a fresh run plays on to the same last line.

That's 31 files and about 17,400 lines of ent-lang. It has the title, the
menus, all five skills, the mouse, pause, gamma and low detail. On 16 October
it became a single Mac binary with the WAD inside. Nothing else in the project
made me as happy as watching that first demo stay in sync to its last tic.

## Pushover: dominoes as entities

On 11 October came the second port: Pushover, the free remake of Ocean's 1992
puzzle game, in which an ant rearranges dominoes so that one push topples
them all. The reference is its upstream at tag 0.0.5, built headless to write
a trace line per tick.

I chose the model: **dominoes are entities, plus a grid of who stands where.**
That needed one more language feature first, an entity as a buffer field, so
`Cells` holds `who: entity` and a domino finds its neighbour without a
search. They take their turns place by place:

```
for e, d: mut Domino in turn by d.cell { ... }
```

A domino that moves to another place becomes a new entity there. If that
place comes later in the tick, it gets another turn in the same tick, just as
in the original, whose recordings depend on that quirk.

All 244 recordings from upstream replay the same as the reference, every
tick, sounds included. When I ran the window, my comment was "looks great, I
want menus". It got menus, sets of levels, a solved list, its pictures
rendered with POV-Ray, and `asset` plus `--embed` to make it one 33 MB file.

## What this settled

- **Games find the holes; the language fills them.** Not the game, and not a
  C device.
- **"ECS doesn't fit X" is a hypothesis.** For Doom the answer was one
  declared sequential `for` and a lot of ordinary systems. For UI it was
  trees with events.
- **A reference beats an opinion.** Every port gets checked against its
  original, tic by tic, pixel by pixel, or value by value. That became the
  default for everything after.

Next: [06 · Science models, matched with their references](06-science-models.md).
