# 10 · Pixalorien: a hackathon game

*27 October – 4 November 2026. In its own repository:
[github.com/mzaks/Pixalorien](https://github.com/mzaks/Pixalorien).*

## The question

By late October ent-lang had ports that matched their references and
benchmarks that matched C. What it didn't have was a game that *only* exists
in ent-lang, with no reference to copy and every design decision our own. Meta's
Global AI Developer Hackathon was the excuse to find out what the language can
do at this stage, against a deadline.

The answer is Pixalorien. Three armoured Pixaloriens land on a desert planet to
fetch a treasure from a vault deep under the surface. It's a single-player game
of real time with pause (and going back) over a world of falling sand. Every
cell of the 2048 × 1024 world is matter that falls, flows, burns and caves in.
Builds exist for macOS, Linux and Windows.

## It started as something else

The day before, it was **EntSourcery**: Frozen Synapse's simultaneous planned
turns crossed with Noita's every-pixel-is-matter, seen from the side. The pixel
automaton survived and the rest pivoted. The new pitch was a cave expedition
with a Mandalorian vibe: a team, jetpacks and grapples, fog lifted only by
light, sealed pockets of gas, water and lava waiting to be cut open.

The plan in `DESIGN.md` had ten steps. The first eight were done in two days:

1. the world;
2. sight;
3. the Pixaloriens;
4. tools and matter;
5. creatures;
6. strategies and rewind;
7. split views;
8. the goal and dying.

That's less heroic than it sounds. Each step was a module, a check program,
and a table of measurements in the README.

## The world, and why ent-lang suits it

The automaton is the part that should have been hard.

- **Sleeping chunks.** EntSourcery's automaton took 11 ms a tick on all cores
  and 50 ms on one at this world size. That's fine at 60 ticks a second.
  It's hopeless for replaying minutes of play in a frame. So the world is 512
  chunks of 64 × 64 cells, each an entity with a window onto the cell buffer.
  Only chunks where something moved, plus their neighbours, are worked. A tick
  with everything asleep costs 0.15 ms.
- **Gather, never scatter.** Every phase (fall, slide, flow, react, wake)
  computes each cell from the cells *as they were*. So **one core and all
  cores give the same world**, checked by digest for every seed.

| | all cores | one core |
|---|---|---|
| making a world | 30–36 ms | 170–185 ms |
| a tick, 47–160 chunks awake | 3.3–7.5 ms | 11–37 ms |
| a tick, all asleep | 0.15 ms | |

Light spreads one cell a tick through what it can pass. Each Pixalorien casts
1,440 rays. Routes are A* over a map of what is *known*, not what is true, so
the team discovers it was wrong about the world at the worst moment. That's
the game working as intended.

## Time, not turns

The first version had five-second turns, from Frozen Synapse. On 29 October
they went. With no opponent planning at the same time, a stop every five
seconds was a ritual nobody needed. Six steps replaced them:

- **T1–T2: time.** 60 ticks a second at speeds 1×, 2× or 4×. Every order is
  logged with its tick. There's a timeline you can drag along. Going back to
  any moment loads the last saved world before it and replays the log without
  drawing.
- **T3: the game stops itself** when something happens: a creature seen, a
  soldier hurt, a cave-in. A big card shows what happened, with GO ON, BACK
  5 S and LOOK.
- **T4: an unforgiving world.** Lava does 25 damage a tick, rubble crushes and
  roofs crumble. In the balance runs, losses went from 5 to 10 out of 24 games.
  That's the point: going back is part of playing.
- **T5: the tutorial teaches time.** A drill sergeant stages a cave-in. The
  heavy's charge into a crumbling roof buries the squad, and the next lesson
  is GOING BACK. The flamer lesson is staged as friendly fire, followed by a
  lesson called FRIENDLY FIRE. Of all the comedy in the game, that one landed
  best.
- **T6: backwards.** An undo log plays the world in reverse, a tick a frame.

All of this rests on one property: **the same world and the same orders at the
same ticks give the same game.** ent-lang gives that for free, on any number of
threads. Without it, a "replay" is just a video.

## Playing it backwards

`journal.ent` writes down, after every tick, what the tick changed and the
value it had before. It finds the changes by comparing with a shadow copy, and
only where something *could* have changed: awake chunks and cells near an eye.
The first version wrote 804 KB a tick, mostly 88,000 rows of "seen again". A
rule that skips cells seen in the last round too cut that to 2.5 KB a tick.

- Logging: 0.7 ms a tick.
- A tick back: 0.27 ms, about 3,700 ticks a second.
- The 128 MB log holds about 14 minutes of play.
- The check goes back 3,600 ticks to the landing and forward again. Every
  tick's digests match.

Doing this by hand showed what the language should do instead. That's open
topic 51, *journaled buffers*: a buffer that records old values where they are
written, with no shadows and no hand-written rules about which chunks might
have changed.

## The re-run postcard

This is the feature I'm happiest with.

A game is its seed and its orders. So **SAVE RUN** on the end screen writes a
`.pixrun` file containing:

- the simulation's version;
- the seed;
- the orders, with their ticks;
- a digest of the world at the end.

A game with a few orders fits in 55 bytes. LOAD plays it back under WATCHING A
RUN, and at the end compares the digests: **VERIFIED** or **NOT THE SAME**.

A file is nice, but nobody shares files. So SAVE RUN also draws a **card**: a
1200-pixel picture of the end screen, with the dig, each soldier's path, who
died where, and a line from the sergeant. Under it sits a band of blocks that
*is* the run file:

- light for 1 and dark for 0, 8 pixels each;
- a row of alternating blocks to calibrate light against dark;
- the length nine times over;
- the bytes in interleaved Reed-Solomon codes that mend 16 wrong symbols in
  255.

A messenger app re-encodes the picture as a JPEG and shrinks it, and the run
survives: tested at 90% with JPEG quality 80, and at 50% with quality 60. Drop
the picture on the game's window, at the title screen, in a menu or mid-game,
and it plays that run and tells you whether it's the same game. COPY CARD puts
it on the clipboard on all three systems. A cropped card is lost, and a run
over 2048 bytes is saved without a card, and the button says so.

So a screenshot of your best game is also the game itself, verifiable by
anyone with the same build. That's determinism turned into a social feature,
and I haven't seen another game do it.

## Everything else a game needs

- **Effects** are drawn from the world and its tick alone, so they play
  backwards too: scan pulses, ripples for cries, sparks, dust, debris and
  embers.
- **Sound effects** come from the game's own mixer: 24 voices made from tones
  and noise, no sound files, streamed at 22,050 Hz. That costs 0.03 ms a frame,
  and each sound is as loud as it is near the views.
- **Music** is "Mist City" by Section7 (CC BY 4.0), cut at its bar lines into
  loops for each situation: title, surface, underground, deep, danger, won and
  lost. A new piece starts on a bar line of the old one and crossfades over two
  seconds so the beats line up.
- **A recruiter's advert** before the tutorial is 45 seconds filmed by the game
  itself on a showcase world: a dam opened, an ice wall melted, a roof brought
  down, a lava vent lighting the gas. It's scripted orders, and the matter does
  the acting.
- **An autopilot** has three strategies (manual, cautious, brute) that you can
  switch at any time. The balance check plays 12 seeds headless per strategy.
- **Art direction** came from Meta's Muse image model: the key art and the
  sergeant's portrait. The sprites are drawn by hand at 5 × 9 pixels, because
  the model drew 40 × 50. The credits name both coding assistants: Claude Code
  and Muse Code.

The landing minigame was built (step 9) and then switched off. Two friends who
played it saw no point to it, since nothing in the game follows from how the
ship comes down. They were right, and it waits in the code for when landing
matters.

## What the language got out of it

The game ran into a list of rough edges, now open topic 50: names colliding
across modules, a text type that depends on the literal's length, a buffer
capacity that couldn't be a constant, a 15-minute clang compile of one `put`
with 120 interpolations. It also changed the language directly:

- **Threads that sleep at once on a Mac.** A tick of the world went from
  1,880–4,290 ms to 403 ms per 50 ticks.
- **Threads that sleep at once in programs with a window.** At the title
  screen, CPU time went from 247 s to 57 s over 20 seconds.
- **`ent build --windows`.** Pixalorien under Wine gives the same digests as on
  Linux.
- **Pictures as files, and files dropped on the window**, for the postcards.

Pixalorien builds against a tagged ent-lang, v0.1.1. That's where the tags came
from: a game needed a version to stand on.

Next: [11 · Other languages behind devices, and v0.1](11-other-languages-and-v0-1.md)
