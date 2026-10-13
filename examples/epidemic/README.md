# A city's epidemic in ent-lang

A model of an epidemic in a city, person by person, after
[OpenABM-Covid19](https://github.com/BDI-pathogens/OpenABM-Covid19): it
is to give that model's curves for its baseline, and then to be faster.
What is here is the illness, the city in which it is passed on (at
home, at work and school, and to whom one meets by chance), and what is
done about it: a lockdown, staying at home with symptoms, tests, and an
app that tells those one has met. Run side by side from one file of
numbers, the model's curves are the reference's, with and without those
measures.

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
  of a day's meetings is made or kept: for each stub of everybody ill,
  the stub next to it and whose that is are found, in a loop over their
  stubs that sends to whomever it finds, on all the cores. In a lockdown fewer ties are met along, fewer people are met by
  chance, and a meeting at home counts for more.
- `interventions.ent`: who has symptoms (of this illness or another)
  stays at home, alone or with their household, and is tested; whom a
  test finds stays longer, whom it does not find need not. Who has the
  app and becomes such a one has those told whom the app saw them meet
  in the last week: their household, the ties of work that were met (a
  number worked out again, as on the day), and whom they met by chance,
  which is worked out again from where everybody's stubs were on each of
  those days (kept for eight days: eight numbers a person, where the
  reference keeps every meeting of ten days). Who is told is from then
  on tied to the one they were told about by an edge (`ToldBy`), stays
  at home if they are of the part that does, and may be tested; each day
  everybody looks along their own edges whether the one at the other
  end is still such a one, or was found by a test meanwhile. All of it
  is `for`s over the people that run on all the cores.
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
against. A day of 100,000 people takes 7.5 milliseconds on one core where
the reference takes 44, and one of a million 107 (32 on all the cores)
where it takes 590, in a quarter of its memory; with every measure on,
108 (44 on all the cores) where it takes 470: `bench/epidemic/RESULTS.md`
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
  `check/curves.py isolation`, `testing`, `household` and `tracing` add
  the measures one after another (`tracing`: all of them, the app on
  from the first day), and hold besides how many are kept at home, for
  how many days, how many tests there are and how many they find. Each
  agrees over 20 seeds; `tracing`, which leaves 33,000 infected of the
  baseline's 86,000, on seeds 21 to 40 in all of it, and on seeds 1 to
  20 in all but the day of the peak (78 where the reference has 88) and,
  with that, the days at home for each infected up to day 100 (63 where
  it has 66.5): with ten infected at the start, when an epidemic that is
  held down takes off is chance, and the model's seeds 1 to 20 have the
  same early days in every scenario.
- `check/same.py` runs the model built for one core and built for all
  of them (on 1, 4, 16 and all threads), each writing for every day a
  number made of everything about every person: the same line for line,
  with 100,000 people and with a million (`same.py 1000000`: only from
  a million on are the loops shared out among the threads), and with
  the measures (`same.py 100000 tracing`).
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
  the seed (as it is for those who are told whom they met). Two
  stubs of one person that come together are no meeting; the reference
  looks for another stub to put there (9 of 341,000 stubs on a day).
- A lockdown is the reference's, by its days in the parameter file:
  less of each network's ties met, fewer met by chance (each person's
  number for the lockdown drawn once), more at home. The reference's
  lockdown of the old alone is not there, nor one that starts when so
  many are infected, which the reference has only in its Python.
- The columns of a day's line are the reference's by their names, those
  the model has.
- Of the reference's measures the model has: staying at home with
  symptoms (also of something else) and giving up; tests on symptoms, on
  going to hospital and for those told, with their waits and what a test
  finds; the household staying at home with one who has symptoms or was
  found; the app, telling on symptoms and on a test, staying at home and
  tests for those told, the second telling when a test finds one whose
  symptoms were told already, and being let go when the one it was about
  is not found or their days end. It has them by the reference's rules,
  read from its source, and its parameter file's names.
- Not there: tracing by people (manual tracing), tracing on from those
  told (the reference's depth is 0 in its baseline), the households of
  those told, tests in order of how many one met, letting go early by
  how many of those told fell ill, vaccines, and the hospitals as
  places.
- What happens to several people on one day happens to all of them at
  once, where the reference goes through them one after another: who
  has symptoms, then the tests, then who is told, then who is let go.
  Where the reference's order decides (a test that clears somebody on
  the day another lets them go), it is either as likely.
- Who has the app is drawn for each person by their age's share, where
  the reference picks exactly so many of each age.
