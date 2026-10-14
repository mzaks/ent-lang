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
  all of them, to the bit.
- `beecs.ent`: a run, a line for each day.
- `reference/`: `fetch.sh` fetches beecs (GPL-3.0, so none of its files
  are in this repository) at the commit the model was held against into
  `build/beecs/reference`, builds `runner/` against it (a Go program
  that runs beecs for a range of seeds and writes the same columns), and
  copies beecs's weather files to `build/beecs/run`.

```sh
examples/beecs/reference/fetch.sh        # once: beecs, built, and its weather
cd build/beecs/run
../../../tools/ent run ../../../examples/beecs/beecs.ent > days.csv
```

The columns: the seed, the day, the worker eggs, larvae, pupae, in-hive
workers and foragers, the drone eggs, larvae, pupae and drones, the
honey [kg] and pollen [g] in store, the age of first foraging and the
day's foraging rounds.

## What is checked

`check/curves.py [scenario] [--seeds N]` runs beecs and the model for
seeds 1 to N (100) from the same parameters and holds the model's days
against beecs's spread from seed to seed: for every column, the model's
median of each day within beecs's 5% to 95%, and each run's mean and
peak spread alike (Kolmogorov-Smirnov at 1%); where colonies may die
out, as many lost (Fisher at 1%). Four scenarios agree in all of it:
`default` (a year of beecs's defaults), `years` (three years, the
weather of 2000 to 2002), `seasonal` (two patches whose flowers follow
the season) and `lean` (a small, poor colony with one far patch, two
years, ending where it dies out). `default`, `seasonal` and `lean` also
agree over 400 seeds; on `lean` 21 of 400 colonies die out in beecs and
18 in the model.

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
