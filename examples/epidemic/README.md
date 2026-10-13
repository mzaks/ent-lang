# A city's epidemic in ent-lang

A model of an epidemic in a city, person by person, after
[OpenABM-Covid19](https://github.com/BDI-pathogens/OpenABM-Covid19): it
is to give that model's curves for its baseline, and then to be faster.
It is being built in steps; what is here is the illness, and the city
with its households and its networks of work and school, in which it is
passed on. The random meetings of each day are still to come.

- `random.ent`: numbers at random without a generator that is kept. A
  draw is worked out from what it is for (the seed, the person, the day,
  the purpose), so a `for` that draws runs on all the cores and gives
  what it gives on one. Uniform and normal numbers, gamma waits, and an
  order of the numbers below a count in which each is found by itself
  (`permute`), to pick so many different people or to shuffle a ring.
- `disease.ent`: the people (`Person`, `Age`, `Disease`, and the tag
  `Hospitalised` for as long as they are in hospital) and the illness.
  A person's `Disease` holds the stage they are in, the one that
  follows and the day it does; `progress` moves on everybody whose day
  it is and draws what follows, by the tables `ages` (which way, by
  age) and `delays` (how long). `tally` counts the stages.
- `city.ent`: the households, taken from the reference's panel of
  10,000 so that the city's ages and household sizes come to the
  country's; a person lives in one by an edge of the tree `LivesIn`. And
  the five networks, each a ring of its members rewired as the
  reference rewires it: a tie is one edge of `WorksWith`, met along
  from either end.
- `transmission.ent`: what an ill person gives off on a day, and whose
  hazard it is taken from. At home the household adds up what its ill
  give off (up the tree) and every member loses it (back down), in place
  of everybody meeting everybody; at work it goes along the ties that
  are met that day, which a number worked out from the tie and the day
  says.
- `epidemic.ent`: a run on the console, a line of counts for each day.
- `view.ent`: 10,000 people in a window, a dot each.

```sh
examples/epidemic/reference/fetch.sh     # once: the reference, into build/
cd build/epidemic/run                    # where its household panel is
../../../tools/ent run --parallel ../../../examples/epidemic/epidemic.ent > days.csv
../../../tools/ent run --parallel ../../../examples/epidemic/view.ent
```

The reference's files are under the GPL and are not in this repository:
`reference/fetch.sh` fetches them at the commit the model was held
against. 100,000 people and 183 days take 0.7 seconds on one core.

## What is checked

- `check/disease.py` infects 100,000 people at once and holds every
  step of every illness against the tables: the shares of each way by
  age, and the mean and the deviation of each wait (101 numbers, each
  within four standard errors).
- `check/city.py` builds a city of 100,000 and holds it against the
  reference's targets (44 numbers): ages, household sizes, who is in
  which network and how much staff, the ties each member has and how
  they are spread (against the reference's own rewiring, done in the
  script as its C does it), and what a day's meetings at work and at
  home come to.
- A run is the same to the byte on one core and on all of them
  (`test/Integration/epidemic_disease.test`, `epidemic_city.test`).

## As the reference has it, and not

- The stages and the ways between them are the reference's, with its
  numbers by age and its waits (held against its source and its
  baseline parameters). A wait is a gamma number rounded to the nearest
  day, one at least; the one from symptoms to hospital is five days or
  six.
- Somebody critical who is not taken into intensive care dies on the
  day they become critical, as there.
- Households are taken as the reference takes them (four at a time,
  kept where they fit), by this model's own draws.
- A network's ties are drawn tie by tie, each by itself, where the
  reference goes through a ring and changes it as it goes. The rule is
  the reference's (each end's turn, and another for a tie moved to a
  place yet to come); what is left out is its look whether a moved tie
  doubles one that is there, which on a ring of thousands hardly
  happens.
- What is given off along one meeting is the reference's rate shared
  among the meetings a person has on a day, on average over everybody.
  Without the random meetings that average is smaller than it will be,
  so a meeting counts for more here than in the whole model.
- The numbers are tables and consts in the source; they are to be read
  from the reference's parameter file when the two are run side by side.
- The columns of a day's line are named plainly, not yet as the
  reference's time series names them.

## Still to come

The random meetings of each day, the runs against the reference, the
large sizes, lockdown and isolation, and then testing and tracing.
