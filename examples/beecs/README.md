# A honeybee colony in ent-lang

[beecs](https://github.com/mlange-42/beecs) in ent-lang: BEEHAVE's model
of a honeybee colony and its foraging, which beecs wrote in Go on the
Ark ECS. A colony through the year: the queen lays eggs by the season
and by how many nurses there are, brood grows into in-hive bees and
those into foragers, which fly out in squadrons to flower patches for
nectar and pollen, dance for good patches, and die of age, of flying and
of what the colony lacks. Run side by side for many seeds, the model's
days spread as beecs's do.

- `parameters.ent`: beecs's parameters as uniques of the same names
  (`Foragers.squadron_size` is beecs's `Foragers.SquadronSize`), with its
  defaults, and read from a file `parameters.txt` where the program is
  started: a line `Name.Field value` for each that is to be another,
  flower patches as lines of their own. And the weather, the hours for
  foraging on each day of the year, read from beecs's files.
- `colony.ent`: what beecs keeps as arrays of cohorts (eggs, larvae,
  pupae, in-hive bees, a number for each day of age, of workers and of
  drones) as the rows of one buffer, and the systems that work on them
  as beecs's do: the age of first foraging, the cohorts' mortality,
  ageing, laying eggs, in-hive bees turning into squadrons of foragers,
  counting, brood dying for want of nurses or protein, and the honey and
  pollen eaten.
- `foraging.ent`: the flower patches and the squadrons as entities, with
  beecs's components, and a day's foraging as rounds, each round a
  schedule of systems (`round`): the patches' trips, decisions,
  scouting, recruits looking for a danced patch, experienced foragers
  going back to theirs, the honey a flight costs, dying on the way,
  dancing, unloading.
- `random.ent`: numbers at random worked out from what they are for (as
  in `examples/epidemic`): the seed, the squadron, the day, the round and
  the purpose. beecs draws from one stream and shuffles lists with it;
  here an order at random is a random number for each squadron, which a
  `for ... in turn by` goes by. So a run is the same on one core and on
  all of them, to the bit. The numbers are wyrand's (`import wyhash`, one
  multiply to 128 bits each), but in the loops that run on eight
  squadrons at a time SplitMix64's, whose multiplies are done eight at
  once.
- `claims.ent`: a round without turns (`Variant.Claims true` in
  parameters.txt): where beecs has the squadrons go one after another
  (taking loads from a patch, following dances), here they ask all at
  once and the patch shares what it has; see below.
- `beecs.ent`: a run, a line for each day.
- `many.ent`, `many_colony.ent`, `many_foraging.ent`: many colonies at
  once, a seed each, without turns, on the GPU where built with `--gpu`;
  see below.
- `view.ent`: the same run in a window. On the left the hive and its
  patches (beecs's patches have a distance and no direction: here they
  lie a golden angle apart), each a ring of the nectar and pollen it has
  left today, and the squadrons as dots by what they do: resting in the
  hive, scouting, flying to their patch and back. On the right the year
  so far: the workers by stage, the honey and the pollen in store over
  bars of what came in each day, and under them the weather (each day's
  hours for foraging) and the months; over the workers, the drones and
  the age of first foraging. A run of more than a year shows a year at a
  time (its number to click), the other years faint behind it. A box
  over the map says why the bees forage as they do today: the stores
  against what the colony wants, the chance that a forager starts, the
  share for pollen, the foragers that died flying. The mouse over the
  charts shows a day's numbers, with what was brought in and eaten and
  the workers that died, and that day in the box; a click goes to that day (on fast, or from the first day again
  with the same seed) and stops there. A click on a patch shows its year:
  what was taken each day, and what there was, the bees that flew there
  and about how many died on the way, and how long a trip there takes. Space stops and goes on,
  Up and Down change the speed, R starts over with the next seed. M runs
  20 other seeds and shows the middle 80 % of them and their median
  behind this run. P opens knobs (how far the patches are, how much their
  flowers give, the bees and the honey at the start) for the next R. S
  saves a picture of the window.
- `reference/`: `fetch.sh` fetches beecs (GPL-3.0, so none of its files
  are in this repository) at the commit the model was held against into
  `build/beecs/reference`, builds `runner/` against it (a Go program
  that runs beecs for a range of seeds and writes the same columns), and
  copies beecs's weather files to `build/beecs/run`.

```sh
examples/beecs/reference/fetch.sh        # once: beecs, built, and its weather
cd build/beecs/run
../../../tools/ent run ../../../examples/beecs/beecs.ent > days.csv
../../../tools/ent run ../../../examples/beecs/view.ent             # in a window
```

The columns: the seed, the day, the worker eggs, larvae, pupae, in-hive
workers and foragers, the drone eggs, larvae, pupae and drones, the
honey [kg] and pollen [g] in store, the age of first foraging and the
day's foraging rounds.

## What is checked

`check/curves.py [scenario] [--seeds N]` runs beecs and the model for
seeds 1 to N (100) from the same parameters (`--claims`: the model
without turns) and holds the model's days
against beecs's spread from seed to seed: for every column, the model's
median of each day within beecs's 5% to 95%, and each run's mean and
peak spread alike (Kolmogorov-Smirnov at 1%); where colonies may die
out, as many lost (Fisher at 1%). Six scenarios agree in all of it:
`default` (a year of beecs's defaults), `small` (squadrons of 10 bees),
`years` (three years, the weather of 2000 to 2002), `seasonal` (two
patches whose flowers follow the season), `scarce` (two small patches
that run out within a day) and `lean` (a small, poor colony with one far
patch, two years, ending where it dies out). All six also agree over 400
seeds (where a test or two at 1% is off in one of them, it agrees over
another 800, `--first 1001`); on `lean` 21 of 400 colonies die out in
beecs and 17 in the model.

## Timings

`bench/beecs/run.py`, in `bench/beecs/RESULTS.md`, on one core: a year of
the defaults takes 20 ms where beecs takes 23; with every forager an
entity of its own (squadrons of one bee, 10,000 to 20,000 entities) 1.06 s
where beecs takes 1.51. Without turns (below) 10 ms and 0.60 s.

What a round costs is the work for each squadron (numbers at random,
which of its states it is in), not memory: the squadrons are some
hundred kilobytes. So it costs what is done for each, and how often a
loop goes through all of them:

- A number at random is drawn where it is asked for, not before, and
  those every squadron draws, in a loop of their own, which goes eight at
  a time (`Draws`). wyrand's numbers (one multiply each) everywhere but
  in such loops, where SplitMix64's multiplies are done eight at once.
- What is the same for all who go to a patch is worked out once for the
  patch (the chance to give it up, a dance's chance to lead none).
- A value is sent to a field, or rows given to a buffer, from one place
  in a `for`: from more than one, each is kept for every row and put
  together after the `for`, in a fixed order.
- Without turns, what follows a round's flights is two passes over the
  squadrons, not seven (`fly`, `come_home`).

## Without turns

Four steps of a round go through the squadrons one after another, in an
order at random: recruits and experienced squadrons taking loads from a
patch, which may run out, and dancers taking followers from the resting,
each of whom follows one. Such a `for` runs on one core, and is half of
the time where every forager is an entity of its own. `claims.ent` does
these steps without turns:

- Every squadron going to a patch asks it for a load, and the patch
  counts what it is asked. Where it has enough, all get one; where not,
  the recruits first, as in beecs, and of those that cannot all have one,
  as many as there are loads at random. That is what beecs's order at
  random comes to; only its experienced squadrons go in the order they
  are stored, and here they are drawn too.
- Each dancer says how many it would lead, and the resting line up. As in
  beecs, the dancers in an order at random lead theirs while so many of
  the resting are left, and so many of the resting at random follow each
  dance; what a follower does then depends only on the dance's patch,
  and all decide at once. This is one after another too, but over short
  lists (the dancers, the resting) of a few numbers each, with no sort.
  (A first try, each resting squadron following by a chance of its own,
  as many as are led on average, was a little off over 800 seeds: more
  are led than rest in a third of the rounds, and there beecs's rule
  counts, and so do the exact numbers.)

It agrees with beecs as the model with turns does (`check/curves.py
--claims`: `years` over 100 seeds, the others over 400, `seasonal` and
`scarce` also over 800), and is faster on one core: a year of the
defaults takes 10 ms, with squadrons of one bee 0.60 s (with turns 1.06).
On all cores it
gains nothing at these sizes: a round's loops over some 8,500 squadrons
are too little work to share out.

## Many colonies at once

A colony is a few thousand squadrons, too few to give a GPU work: a
round of them on the GPU is a handful of launches of a few microseconds
each, about what the whole round takes on one core. Many colonies side
by side are not: `many.ent` runs `Colonies.Count` colonies (100 by
default), colony c with the seed `RandomSeed.Seed` + c, each as
`beecs.ent` runs one without turns, and says the same lines for each
(so its lines are those of as many runs, one seed each, but for which
numbers each draws). A round's systems go through the squadrons of all
colonies together, and are `gpu system`s: on the GPU in a `--gpu`
build, the same to the bit as on the CPU.

```sh
echo "Colonies.Count 100" >> parameters.txt
../../../tools/ent run --gpu ../../../examples/beecs/many.ent > days.csv
```

What is a unique in `beecs.ent` is a component of the colony here
(`many_colony.ent`): its stores, its counts, its day, and a seed of its
own. The cohorts are a buffer, a colony's rows after another's, and go
through one colony after another (`in turn by`), on the CPU, once a
day. A round (`many_foraging.ent`) is so that nothing in it waits for
the CPU, nor depends on the order the GPU goes in:

- A colony's squadrons have places of their own (`Colonies.Slots`, two
  and a half times its first bees by default), all spawned with it: a
  squadron that dies is dead in its place, and a new one takes a free
  place of its colony. Nothing is spawned or destroyed after setting
  up, so the places lie colony after colony. (A squadron that finds no
  place is counted and said at the end.)
- What the squadrons add up for their colony or a patch is counted in
  whole numbers: who flew where, who brought what home, who would lead
  how many. A flight's honey and seconds are a patch's, so the colony
  works them out from the counts.
- The lists a round makes (who asked a patch that is short, who dances,
  who rests) are running sums over the squadrons, so each colony's part
  is in one piece, in the order of its places.
- Who of the resting follows a dance: the places in the colony's line
  stirred into an order at random that gives each its own place (a
  bijection, so as many follow as are led, exactly), the first of them
  the first dance's, and so on, each squadron for itself. Where fewer
  rest than the dancers would lead, the colony goes through its dancers
  in such an order and each leads where so many are left, as in
  `claims.ent`.

It agrees with beecs as `claims.ent` does (`check/curves.py --many
[--gpu]`: all six scenarios over 100 seeds, `default` also over 400;
over seeds 1 to 100 of `default` the in-hive workers' peaks are off at
1%, and `claims.ent`'s are too). Where every forager is a squadron of its own, 100
colonies take 8.9 s on the GPU (a laptop's RTX 4050), 186 s on one
core and as much on four threads, where 100 runs of `beecs.ent` without
turns take 90 s one after another and 23 s four at a time on four
cores. A round is a few passes over all places, so it costs the memory
they read: some 2 ms a round for 2.5 million places.

## What follows beecs, and what does not

The systems run in beecs's order (`model/default.go`), with its numbers,
also where beecs has a quirk: a squadron that gives up its pollen patch
forgets its nectar patch (`decisions` in `sys/foraging.go`), and the
squadrons going back to a known patch take their turns as they were
made (beecs: as its storage has them), not shuffled.

Not here:
- patches whose flowers follow a script (beecs's `ScriptedPatch`), and
  patches read from a file of their own (`InitialPatches.File`);
- a seed of 0 or below meaning one at random: here it is a seed;
- beecs's observers besides the day's line (by age, foraging statistics
  per round, patch visits), its experiments and its user interface.

The numbers at random are not beecs's (Go's PCG stream and gonum's
Poisson), so no run is beecs's run: the two agree in how runs spread.
