# Traffic on a ring road in ent-lang

Cars on a road that is a ring, one lane, after Nagel and Schreckenberg
(1992): cells of 7.5 m, whole speeds in cells a step, and every step each
car speeds up, keeps behind its leader, brakes at random and moves on.
It is the first step towards roads of several lanes with lane changes,
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

```sh
tools/ent run --parallel examples/traffic/flow.ent > flow.csv
examples/traffic/check/flow.py
```

`test/Integration/traffic_ring.test` runs a few roads and checks the
flows without braking to the cell.
