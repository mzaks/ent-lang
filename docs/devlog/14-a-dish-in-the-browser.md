# 14 · A dish of neurons, playing Pong in your browser

*16 – 21 November 2026.*

## Neurons behind an API

In 2022 Cortical Labs published
[DishBrain](https://doi.org/10.1016/j.neuron.2022.09.001): a culture of
neurons on a multi-electrode array, playing Pong. The ball's place was
stimulated into a few electrodes, the paddle followed the activity of
others, and the paper reported that the culture played better within
minutes. Cortical Labs now sells the hardware as the CL1 and publishes a
Python SDK for it, `cl-sdk`, with a simulator. That simulator replays a
recording: the program runs, but the "neurons" neither answer a stim nor
learn.

So we put a dish of our own behind the SDK. `examples/cl_bridge/network.ent`
is 1,024 leaky integrate-and-fire neurons placed at random on a square,
wired to their neighbours through a grid index, with 64 electrodes, STDP,
adaptation and axons. `ent build --shared` was new for it: a library of a
program without `main`, which a host loads and drives through a flat C
interface (`bridge.c`). A Python data source hands its spikes to the SDK,
and a program written against the public CL API (`cl.open`,
`neurons.loop`, `neurons.stim`) then talks to neurons that answer.

The first thing I wanted was for it to look like a culture. Wagenaar, Pine
and Potter published 2006 recordings of cortical cultures on the same
kind of array, so we fitted the dish's 16 parameters to 12 statistics of
theirs: firing rate, bursts, the answer to a stim. Ten of the twelve land
in the cultures' middle half. That is a fit, not a validation, and the
dish was never compared with a CL1. The README says so at the top, and
every page that shows it says so too.

## What it learnt, and what it didn't

Most of the week was studies, each with a control:

- **DishBrain's own feedback** (a predictable stim after a hit, a random
  one after a miss, plain STDP): nothing learnt, on soft or hard bounds.
  The model has no mechanism that rewards predictability.
- **A reward channel,** two electrodes that deliver a dopamine-like signal
  to reward-modulated STDP. No culture has this; that's the point of
  stating it every time. A reward for each paddle move towards the ball
  takes the dish from 0.43 to 0.80 hits a rally (+0.37 over gain 0, 16
  of 16 pairs); the same pulses with coin-flip outcomes teach nothing.
- **The dish's own surprise as the reward,** after reading Active
  Inference: twice nothing, once as a steady dose, once as a pulse.
- **What the reward teaches.** A probe after training stims each ball
  electrode alone: the up-minus-down answer slopes from "up" at the top to
  "down" at the bottom, +9.6 spikes over the untrained dish, and the seeds
  that gained more of that map gained more hits. It's a map, not a habit.

Claude ran the studies and wrote the README sections; I picked what to
try next. The nulls are in the README with the same care as the wins,
including which settings were chosen after a null result. That part
matters more for a dish than for a crawler: here the honest answer is
often "the model can't say".

## "Can I actually see it?"

The studies printed tables. I asked to see a game. The first answer was
a recording: pong.py writes the ball, the paddle and every electrode's
spikes and stims every 20 ms, and a page replays two of them side by
side, untrained and trained, the 8 × 8 array glowing where it spikes.

Then the obvious next questions: could people run their own seed, and
could the simulation run in the browser, with the seed typed as any text?
It could, but not the way it ran. The studies go through `cl-sdk`, which
is Python and won't run in a page. So three pieces had to change hands.

**The game moved into C.** `game.c` is pong.py's game (the columns
layout, the reward for each move or for hits and misses) next to the
dish: each tick it runs the dish 5 ms, counts the motor regions' spikes,
moves the paddle and the ball, and queues the next stims. A host calls it
once per 20 ms frame. Natively that host is `dish_pong.py` through ctypes;
in a browser it's a worker.

**Same seed, same game, everywhere.** For that the game can't touch the C
maths library. glibc's `tan` and Emscripten's musl `tan` may differ in
the last bit, and a last bit is enough to send a ball elsewhere a minute
later. So the game has its own generator (splitmix64) and its own `tan`
and `exp`, as series in plain arithmetic. ent already compiles every C file
with `-ffp-contract=off`, so no multiply and add get fused differently on
different machines.

**`ent build --shared --web`.** On the web, a "shared library" is an ES
module: `dish.mjs` and a 75 KB `dish.wasm`, exporting the C functions
that come with the program. The first check made a dish natively and in
Node.js, gave both the same stims for 10 s, and compared checksums:
`0xbf2cb6beec92043a` both times. The second played 12 s of a seed in both
and compared every frame, every spike and every stim; that one is now
`cl_bridge_web.test`. And the WebAssembly dish runs as fast as the native
one: 3.7 times real time on one core, so two dishes side by side play in
real time with room to spare.

The text seed is FNV-1a of the text's UTF-8, its low 31 bits, the same in
Python and JavaScript. "hello dish" is 945407425, and anyone can type it
on the page or give it to `dish_pong.py` and get the same games.

One thing to check before calling it done: the browser's loop no longer
goes through the SDK's timing, so does it still learn? Eight seeds, 20
minutes: 0.43 to 0.85 hits a rally after minute 5, +0.43, all eight up.
"hello dish" goes from 0.41 to 0.91.

## Pac-Man, with two worlds on one page

The dish had also been steering Pac-Man: `pacman_dish.ent` stims where
the nearest pellet and ghost are, and takes the way from whichever motor
region fires most. Nothing is learnt there; the wiring does it, and with
the stims off he scores half as much. Natively the pilot loads
`libdish.so` with `dlopen` and `RTLD_DEEPBIND`, because the game is an ent
world too and the two would otherwise share symbol names.

A browser has no `dlopen`. But it has no trouble with two WebAssembly
modules either: each has its own memory, so there's nothing to clash.
The pilot's C now calls the dish through a few `EM_JS` functions, which
copy each millisecond's spikes from the dish's memory into the game's. A
`--pre-js` loads the dish before the game's `main` starts and reads
`?seed=` from the address. The one change to `tools/ent` was
`$ENT_MODULE` in a `.web.link`, so a module can name a script next to it.

## Dark mode, on a Mac

The pages went onto the [demos site](https://mzaks.github.io/ent-lang/):
[Dish Pong](https://mzaks.github.io/ent-lang/dish-pong/) and
[Pac-Man steered by a dish](https://mzaks.github.io/ent-lang/dish-pacman/).
The first report was that Pong worked fine in light mode and showed
nothing in dark mode on macOS: a dark court, no paddle, no ball.

In Chromium, forced into dark mode, it looked right. We couldn't run
Safari here. What the page did was read its colours from the stylesheet into
JavaScript once and paint the court, the ball and the paddle with them,
which works as long as every browser answers that question the same way
at that moment. So the fix doesn't depend on it: the court's ground is
now the canvas's own CSS background, the colours are read again every
frame, and the ball and the paddle each wear a ring of the other's
colour, visible on any court. The next report was "it looks great on
WebKit now". We still don't know which part fixed it, which is a little
unsatisfying.

## What's still open

- The plateau. Learning happens in the first few minutes and then stops,
  even though the map is real. The lead: the map's edge is about 10 of
  the 33 spikes a stim draws, while the motor regions fire 118 Hz of
  their own, so the dish's own activity outweighs what the ball steers.
- DishBrain coded the ball relative to the paddle, by the account of a
  later paper from the same group. We tried it: it learnt less. It still
  needs checking in the 2022 paper itself.
- Whether a real culture separates DishBrain's predictable and
  unpredictable feedback where the model can't. Only a CL1 can answer that.

## What this settled

- **One implementation beats two that agree.** Porting pong.py to
  JavaScript would have meant two games to keep in step. Moving it to C
  next to the dish gave one game and two hosts.
- **Determinism across platforms is a choice of maths.** The dish was
  already bit-identical as WebAssembly. The game became so once it
  stopped calling libm.
- **A browser is a fine lab bench.** The dish runs as fast there as
  natively, and anyone with the link can type a seed and watch it learn.

Back to the [index](README.md).
