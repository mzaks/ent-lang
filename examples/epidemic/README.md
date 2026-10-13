# A city's epidemic in ent-lang

A model of an epidemic in a city, person by person, after
[OpenABM-Covid19](https://github.com/BDI-pathogens/OpenABM-Covid19): it
is to give that model's curves for its baseline, and then to be faster.
It is being built in steps; what is here is the illness, and the city
in which it is passed on: at home, at work and school, and to whom one
meets by chance, with a lockdown. Run side by side from one file of
numbers, the model's curves are the reference's. Still to come are the
large sizes, and testing and tracing.

- `random.ent`: numbers at random without a generator that is kept. A
  draw is worked out from what it is for (the seed, the person, the day,
  the purpose), so a `for` that draws runs on all the cores and gives
  what it gives on one. Uniform and normal numbers, gamma waits, and an
  order of the numbers below a count in which each is found by itself
  (`permute`), to pick so many different people or to shuffle a ring.
- `parameters.ent`: the model's numbers, the reference's baseline as
  tables, and read from a file as the reference reads its own (a line of
  names, a line of numbers): by age, by wait and by network as rows of
  buffers that a `fn` is given, the rest as fields of a unique.
- `disease.ent`: the people (`Person`, `Age`, `Disease`, and the tag
  `Hospitalised` for as long as they are in hospital) and the illness.
  A person's `Disease` holds the stage they are in, the one that
  follows and the day it does; `progress` moves on everybody whose day
  it is and draws what follows, by the numbers of `Ages` (which way, by
  age) and `Delays` (how long). `tally` counts the stages.
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
  says. Whom one meets by chance is new each day: everybody who is up
  has a stub for each meeting, one after another in the order of the
  people (a running sum over them, on one core), and an order of all the
  stubs that the day says pairs them. That order is worked out, each way
  (where a stub comes in it, and which stub comes at a place), so nothing
  of a day's meetings is made or kept: a proc finds, for each stub of
  everybody ill, the stub next to it and whose that is. In a lockdown fewer ties are met along, fewer people are met by
  chance, and a meeting at home counts for more.
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
against. A day of 100,000 people takes 6 milliseconds on one core where
the reference takes 44, and one of a million 90 (53 on all the cores)
where it takes 590, in a sixth of its memory: `bench/epidemic/RESULTS.md`
has the numbers, up to ten million people, and what they are made of.
A file "parameters.csv" where the program is started gives it other
numbers: the seed, how many days, a lockdown, any of the reference's
that the model has.

## What is checked

- `check/disease.py` infects 100,000 people at once and holds every
  step of every illness against the tables: the shares of each way by
  age, and the mean and the deviation of each wait (101 numbers, each
  within four standard errors).
- `check/city.py` builds a city of 100,000 and holds it against the
  reference's targets (52 numbers): ages, household sizes, who is in
  which network and how much staff, the ties each member has and how
  they are spread (against the reference's own rewiring, done in the
  script as its C does it), how many each meets by chance, and what a
  day's meetings at work, by chance and at home come to.
- `check/curves.py` runs the model and the reference, built from its
  C, for 20 seeds each from the same parameter file, and holds the
  model's runs against the reference's own spread from seed to seed:
  the median of each day's new infections inside the reference's 5% to
  95%, and the day of the peak, its height and how many were infected
  in all spread alike (a Kolmogorov-Smirnov test at 5%).
  `check/curves.py lockdown` does the same with a lockdown from day 20
  to day 60 and 500 infected at the start, so that every run is as far
  along when it begins. Over 120 seeds of each the baseline agrees in
  all of it; of the first 20 alone the peaks' heights are just outside
  (0.45 where 0.43 is allowed), which the next 60 are not: such a test
  fails one time in twenty by itself.
- `check/same.py` runs the model built for one core and built for all
  of them (on 1, 4, 16 and all threads), each writing for every day a
  number made of everything about every person: the same line for line,
  with 100,000 people and with a million (`same.py 1000000`: only from
  a million on are the loops shared out among the threads).
- So is every run of the tests (`test/Integration/epidemic_disease.test`,
  `epidemic_city.test`).

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
- The stubs of the meetings by chance are paired in an order worked out
  from the day, where the reference shuffles them, and the meetings of a
  day are not kept: who met whom on any day can be worked out again from
  the seed (which is what tracing will need of the days before). Two
  stubs of one person that come together are no meeting; the reference
  looks for another stub to put there (9 of 341,000 stubs on a day).
- A lockdown is the reference's, by its days in the parameter file:
  less of each network's ties met, fewer met by chance (each person's
  number for the lockdown drawn once), more at home. The reference's
  lockdown of the old alone is not there, nor one that starts when so
  many are infected, which the reference has only in its Python.
- The columns of a day's line are the reference's by their names, those
  the model has.

## Still to come

Isolation on symptoms, and then testing and tracing.
