# 15 · Where a dish stops learning, and a heartbeat

*21 – 25 November 2026.*

## The question left over

[Post 14](14-a-dish-in-the-browser.md) ended on a plateau. The dish
learns Pong with a reward for each paddle move, most of it in the first
five minutes, and then stops, though what it learnt is a real map from
the ball's place to the paddle's way. This week was spent finding where
that stop comes from. It took about thirty studies, most of them with 16
seeds, paired, and the answer is less exciting than any of the guesses
along the way: the stop is in the dish and its learning rule, not in
the game. Getting to that answer took most of the week, and on the way
the dish got a good deal better at Pong.

The loop, for reference. Nothing in it is a state vector handed to a
network; the game can only pulse electrodes and count spikes:

```
 ball ──► its row (8 rows) and how near ──► pulses on that row's sensory
                                            electrode, 4 Hz far to 40 Hz near
                                                      │
                                        dish: 1,024 neurons under 64 electrodes
                                                      │
 paddle ◄── up region's spikes against down region's (every 5 ms)
   │
   └─► moved towards the ball? better than expected: pulse on R
                               worse than expected:  pulse on P
       (R and P are dopamine for the whole dish, not current; no culture has them)
```

## A test that doesn't move

The first thing was to stop measuring a moving target. A dish that keeps
learning while it is scored mixes what it knows with what it is learning
right then. So the dish is now replayed to a minute of its training,
frozen (STDP off) and played on 120 fixed serves, the same for every
seed and setup. The plateau survived that: frozen, the dish is as good
at minute 7 as at minute 20, and as good at 60 as at 20. Seeds do swing
from minute to minute, and those swings are real; the best minute of a
seed beats its minute 20 on a second set of serves for 7 of 8 seeds.
Going back to that best minute and continuing with a slower rate keeps
the map but doesn't better it.

## Where it misses

Then: where on the court. `game.c` counts the balls by the eighth of the
court they arrive in, which is also the sensory electrode that told the
dish about them. With the ball coded as its height on the court, the
reward taught the edges and never the middle (rows 3 and 4 at 0.15-0.22,
as with no reward at all). Which makes sense once seen: a ball in the
middle of the court wants the paddle up or down depending on where the
paddle is, and the dish wasn't told where the paddle is. The reward for
a middle row says "up" as often as "down".

```
 height code: a row is the ball's height on the court
   ball in row 4, paddle above it  → wants down
   ball in row 4, paddle below it  → wants up         one electrode, two answers

 relative code: a row is the ball's height less the paddle's
   rows 0-3 "ball above the paddle" → always up
   rows 4-7 "ball below the paddle" → always down     one electrode, one answer
```

So the ball is now told relative to the paddle, which is also how a
later paper describes DishBrain's code. On the old electrode layout that
learnt *less*. The wiring explained why: each sensory electrode sits on
column 3 or 4 of the array, nearer the "up" or the "down" region, and
its direct synapses lean 1.3 to 1.9 to one towards the nearer side. The
relative code wanted the opposite in four of the eight rows, the four
that stayed wrong:

```
 the old layout                      relative code   grown wiring
          col 0 1 2 3 4 5 6 7        wants           leans
   row 0      U U . S . . D D        up              up    (column 3)   ✓
   row 1      U U . . S . D D        up              down  (column 4)   ✗
   row 2      U U . . S . D D        up              down               ✗
   row 3      U U . S R . D D        up              up                 ✓
   row 4      U U . S P . D D        down            up                 ✗
   row 5      U U . . S . D D        down            down               ✓
   row 6      U U . . S . D D        down            down               ✓
   row 7      U U . S . . D D        down            up                 ✗

   U up region (columns 0-1)   D down region (6-7)   S a row's sensory electrode
   R reward, P punish          column 3 is nearer U, column 4 nearer D
```

Training raised a row's up and down weights together
and moved their balance a few per cent, nowhere near enough to undo two
to one. Moving the electrodes so each row sits on the side it wants
(the *aligned* layout; the host picks electrodes, which a CL1 user could
do too) and reading each motor region against its own background
(decoder 1) gave the setup the page now starts with: **0.71 of the balls
hit**, 0.49 with no reward, every row of the court learnt.

```
 the aligned layout
          col 0 1 2 3 4 5 6 7
   row 0      U U . S . . D D        "ball above": on column 3, nearer U   ✓
   row 1      U U . S . . D D                                              ✓
   row 2      U U . S . . D D                                              ✓
   row 3      U U . S R . D D                                              ✓
   row 4      U U . P S . D D        "ball below": on column 4, nearer D   ✓
   row 5      U U . . S . D D                                              ✓
   row 6      U U . . S . D D                                              ✓
   row 7      U U . . S . D D                                              ✓
                                     (punish moves to 35; dopamine is for
                                      the whole dish, wherever it goes in)
```

## The audit

Everything after that was trying to break 0.71, each with a control:

- **Train longer:** 40 and 60 minutes, no better.
- **Practice the weak spots:** serving from the far side makes things
  worse, and doesn't rewire the far rows.
- **Two electrodes a row:** a stronger map from the start, less learnt.
- **Smaller motor regions:** the same.
- **Motor regions next to the sensory electrodes:** 0.93, but 0.89 with
  no reward. That is a reflex built by the choice of electrodes, not
  learning, and it is now a separate card on the site ("Dish Pong,
  wired"), labelled as such:

  ```
   "wired" (pong_motor 3)
            col 0 1 2 3 4 5 6 7
     row 0      . . U S . D . .      a stim's own reach and a few synapses
     row 3      . . U S R D . .      carry each row into the region beside
     row 4      . . U P S D . .      it: the host's choice of electrodes
     row 7      . . U . S D . .      plays, the dish needn't learn
                                     (rows 1-2 as row 0, 5-6 as row 7)
  ```
- **DishBrain's own feedback** (a predictable stim after a hit, noise
  after a miss), on 32 seeds: +0.017 ± 0.011 over none. A stronger jolt
  after a miss: nothing.
- **A world model of the dish as the dopamine,** the Active Inference
  idea once more: it rewarded *misses*, because the pause after a miss
  is the most predictable moment the dish has. The dark room problem,
  measured.

We also played DishBrain's game as their data describe it (ball speed,
serve, the 8 s pause, their measure). With their feedback our dish goes
from 0.78 to 0.97 hits a rally, about what their mouse cultures did.
With our reward it plays 3.0, far above any real group. That's not a
claim about real neurons; the reward channel is something no culture
has, and every page says so.

## Dithering

An oracle, the host steering the paddle by what the dish was told,
hits every ball. So the code, the paddle's speed and the stims' timing
all allow it; the gap is the dish's steering. Sorting every miss by what
the paddle did in its last second: 63% are *dithering*, the paddle
turning 7 times a second and getting nowhere.

Smoothing that out (a rectified decoder, hysteresis) does stop the
dithering, and scores worse. The misses move to a paddle that holds the
wrong way. Turning often was the symptom, not the flaw. The flaw showed
up when each sensory electrode was pulsed alone on a trained dish: 15
motor spikes within 10 ms, leaning the right way 0.87-0.90 of the time
in the middle rows. In play, at up to 40 Hz near the paddle, the same
answer is about 5 spikes and 0.65-0.72 right. Short-term depression
drains the paths under any stream of stims faster than about 1 Hz. The
map is good; the stream wears it down when it matters most.

That suggested two things: the noise drowns a weak answer, and a bigger
dish (more neurons under each electrode) would fix it. Neither turned
out to be the limit, but the second took a detour.

## Sixteen thousand neurons, and a bug

The first size study scaled the dish the way the README said bigger
dishes are built, the same culture denser, and ran 1,024, 4,096 and
16,384 neurons, the 16k games on the GPU (13 minutes a game against two
hours on one core). The 16k dish stopped dithering, went the wrong way
instead, and learnt almost nothing (+0.06). Its culture statistics
looked wild: 62 Hz a channel, a burst about every second. So we
tuned the big dish before trusting any of that.

Tuning started with measuring, and the measuring said the neurons were
fine: 1.3 to 1.5 Hz each at every size. An electrode at 16k simply
heard 46 neurons instead of 3, and the denser wiring was local, so a
burst reached a seventh of the array. The 1,024 culture with more
neurons is the reach kept, the chance of a synapse lowered and the
electrodes' hearing shrunk:

| | synapse reach | chance of a synapse | an electrode hears | a channel fires | a burst reaches |
|---|---|---|---|---|---|
| 1,024 | 0.2 | 0.3 | ~3 neurons | 3.6 Hz | 0.73 of the channels |
| 16,384, denser | 0.05 | 0.3 | ~46 | 62 Hz | 0.14 |
| 16,384, tuned | 0.2 | 0.019 | ~3 | 3.5 Hz | 0.68 |

(About 70 synapses a neuron in all three; lengths are in units of the
array's side.) The tuned dish answers a stim as the small one does,
without touching the stims: a stim reaches 16 times the axons, an
electrode hears a sixteenth of the neurons.

Before rerunning, a quieter problem. The study runner sets a dish's
parameters by name through `game.c`, which kept the first eight for the
life of a process and silently dropped the rest. A pool's worker plays
many games, so after its fourth it played everything with its fourth
game's settings. The size study escaped it by luck (the rerun 4k games
came out bit for bit the same), the depression study didn't, and was
rerun (its conclusion held). Fixed: a name set again replaces the old
value, and every game clears them first.

The tuned dish, 8 seeds:

| neurons | hit | no reward | reward adds |
|---|---|---|---|
| 1,024 | 0.72 | 0.49 | +0.23 |
| 4,096 | 0.73 | 0.58 | +0.15 |
| 16,384 | 0.70 | 0.70 | +0.00 |

A bigger dish plays better untaught, the reward takes every size to the
same 0.7, and it still dithers. Not the size, not the noise of a few
neurons under an electrode.

## One stim, one answer

The last check took the game away. The one-stim task from the first
week (a stim on one sensory electrode, the answer in the next 100 ms,
the reward right after) was run with the best setup, on a neutral
layout, on the aligned one (the picture above), and on the aligned one
with the map turned against its wiring (rows on column 3 must now answer
"down"):

| | no reward | best reward | the reward adds |
|---|---|---|---|
| neutral layout | ±0.02 | +0.34 | +0.34 to +0.37 |
| aligned, with its wiring | +0.38 | +0.64 | +0.25 |
| aligned, against it | −0.38 | +0.17 | +0.55 |

(A score of 1 is always right, 0 is chance.) The reward moves the answer
a bounded way from wherever the grown wiring puts it and never turns the
wiring over, with no paddle, no stream of stims and the outcome 100 ms
away. Pong's 0.7 is where this dish, with reward-modulated STDP, stops
on any task of this kind. What would move it is a different learning
rule, which is a question about the model, not about a CL1.

## A heartbeat

In between, a side trip I had queued for after Pong: an article about a
CL1 classifying heart rhythm, atrial fibrillation against normal, 55%
from one example of each against 48% for "silicon", 87% from all of
them. The neurons were used as a fixed reservoir with a trained linear
readout. The obvious missing control is the same design on something
that isn't alive. So `heart.py` plays 10-second windows of real beats
(PhysioNet's AF database) into the dish as pulses, frozen and plastic,
eight replicates, and reads the answer the article's way and three
others:

- read as the article read it (the second after the pattern), our dish
  is at chance: it keeps nothing a second on. Their neurons did, which
  is interesting in itself;
- read during the pattern, the frozen dish gets 0.67 from one example
  (the pulses themselves 0.59) and 0.81-0.82 from all, level with the
  pulses;
- the plastic dish, read per channel, is at chance: forty minutes of
  STDP move the counts more than the rhythm does;
- and one number a cardiologist would look at, how much the gaps between
  beats vary, gets 0.83 from one example and 0.89 from all, ahead of
  every reservoir, the CL1's included.

## Pac-Man got Pong's lessons

Pac-Man's dish is taught nothing (no reward); its layout plays. Four motor regions sit
on the ring around the middle of the array, and the middle 4 × 4 are the
sensory electrodes: one for each way the nearest pellet lies, beside
that way's region, and one for each way the nearest ghost lies, beside
the *opposite* region, so a ghost's stims turn him away. Nearness is the
rate, 4 Hz far to 40 Hz near, as with Pong's ball:

```
          col  0   1   2   3   4   5   6   7
   row 0       .   .   .   .   .   .   .   .
   row 1       .   .   U   U   U   U   .   .        U D L R  motor regions
   row 2       .   L   .  f↑  g↓   .   R   .        f↑  "food is up", beside U
   row 3       .   L  f←   .   .  g←   R   .        g↓  "a ghost is down", beside U too:
   row 4       .   L  g→   .   .  f→   R   .            away from it
   row 5       .   L   .  g↑  f↓   .   R   .
   row 6       .   .   D   D   D   D   .   .        the way: the region firing most
   row 7       .   .   .   .   .   .   .   .        against its own 2 s background
```

That's the fixed layout. By default the host now picks the sensory
electrodes per dish: at the start each of the middle 16 is pulsed alone
four times, and each role takes the electrode on its side that leans
hardest the right way (seed 1's "food is down" electrode had driven no
region at all, so he sat beside pellets below him).

So the pilot took the aligned-wiring idea, the motor regions moved in to
the ring touching the sensory electrodes (they were two deep), the
background decoder, and one lesson of its own: the dish answers in
about the time Pac-Man takes to cross a tile, so it is told the *next*
turn on the shortest way to the food, not the current one. That alone
took him from 1955 to 3287 a game. A self-preservation mode (when a
ghost is close, head for the region with the most room) lifts the score
when the host plays it and is a wash with the dish: the ghost stims'
turn-away already gives most of it. It's on the page as `?flee=`, off
by default.

## What this settled

- **A ceiling needs a test that cannot move.** Frozen evaluation on
  fixed serves was the change that made every later number comparable.
- **Measure before tuning.** The "wild" 16k culture was the electrodes
  hearing too much, not the neurons doing too much. The fix was three
  numbers, not a calibration run.
- **Process-wide state in a library outlives the job.** The parameter
  bug was eight lines of C and invisible in every single run.
- **Controls are the work.** The yoked open loop, the oracle, the
  wiring-against-map test and the heart baselines each changed what a
  result meant; none of them was the experiment anyone set out to run.

Back to the [index](README.md).
