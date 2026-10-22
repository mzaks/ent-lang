# Traffic on a ring road in ent-lang

Cars on a road that is a ring, one lane: after Nagel and Schreckenberg
(1992), in cells of 7.5 m with whole speeds in cells a step, each car
speeding up, keeping behind its leader, braking at random and moving on;
and after the intelligent driver model (Treiber, Hennecke and Helbing,
2000), in metres and seconds. It is the first step towards roads of several lanes with lane changes,
which need the cars in their order by lane and position every step.

- `ring.ent`: the roads and their cars. A car follows the car ahead by an
  edge of `Follows`, so a road is a ring of edges; the cars are placed at
  random, standing. Many roads run side by side, each with its own
  density, top speed and chance to brake, and count how far their cars
  moved. The draws to brake are worked out from the seed, the car and the
  step, so a run is the same on one core and on all of them.
- `flow.ent`: the flow against the density (the fundamental diagram) as
  CSV, for a top speed of 1 and of 5, without braking and with a chance
  of a half.
- `check/flow.py`: holds the flows of `flow.ent` against what is known
  exactly. Without braking every step's flow is min(density * vmax,
  1 - density) (with vmax 1 that is Wolfram's rule 184); with vmax 1 and
  braking the endless road's flow is (1 - sqrt(1 - 4 (1 - p) density
  (1 - density))) / 2 (Schreckenberg, Schadschneider, Nagel and Ito,
  1995), which the roads of 1,000 cells come within 1.4 percent of.

- `idm.ent`: the intelligent driver model on ring roads, in f32, with a
  step of `Sim.dt` seconds in which every car speeds up or brakes by
  the model and moves as one that does so evenly. Each road counts how
  far its cars drove and how often one stopped, and from those comes how
  fast a jam goes round against them.
- `sugiyama.ent`: Sugiyama's experiment (2008), 22 cars on a circle of
  230 m. A jam forms that nothing caused and goes back at 15.3 km/h
  (v0 15 m/s, T 1 s, s0 2 m, a 1 m/s², b 1.5 m/s², cars of 5 m); the
  experiment's went back at some 15 to 20.
- `check/idm.c`: the same model in plain C. Run on the same roads it
  gives what `idm.ent` gives, to the bit: every car's place and speed,
  and what the roads measured.

```sh
tools/ent run --parallel examples/traffic/flow.ent > flow.csv
examples/traffic/check/flow.py
tools/ent run examples/traffic/sugiyama.ent
```

`test/Integration/traffic_ring.test` runs a few roads and checks the
flows without braking to the cell; `test/Integration/traffic_idm.test`
runs two roads of the driver model in ent and in C and compares them to
the bit, on one core and on all.
