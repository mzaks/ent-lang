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

- `lanes.ent`: roads of several lanes. A car drives by the intelligent
  driver model behind the car ahead in its lane and changes lanes by
  MOBIL (Kesting, Treiber and Helbing, 2007), with trucks and drivers
  who would go at different speeds. The car ahead is no longer one edge:
  the cars are in the index `Traffic` by lane and place (docs/syntax.md,
  Indexes), which the program makes again where they moved, and each
  finds its neighbours there.
  With one lane it is `idm.ent`'s model to the bit.
- `check/lanes.c`: the same in plain C, which `lanes.ent` matches to the
  bit, lane changes and all.
- `town.ent`: streets that meet, in one of seven scenes: traffic lights
  where streets of two lanes each way cross (a car that turns left gives
  way to the cars that come the other way), a roundabout of one lane and
  one of two, a priority road of two lanes each way, a stop on every
  street, and a highway of two lanes with an on-ramp and with an
  off-ramp. Streets are roads of one lane on curves, side by
  side where there are more, and a car drives its route over them by the
  intelligent driver model, behind the car ahead on its own road or the
  next ones, behind a car that joins from another road nearer to where
  they meet, and behind one that went the other way where a road splits,
  until it is clear. It changes lanes by MOBIL, as in `lanes.ent`, but
  only to a lane from which a route goes on to where it goes (the inner
  lane to turn left, the outer to turn right). At a junction a car asks
  the light, the stop, whether there is room on the road after, and the
  paths that cross or join its own, worked out from the curves: it gives
  way to a car on such a path, or coming onto it within its gap of time,
  and at the stops to whoever stopped first. The cars from the ramp drive
  on a lane that ends, and change into the highway where they safely can,
  more boldly the less of it is left; one that stands at its end is let
  in by the next car on the highway, a car at a time. A route says with
  each road how many lanes a car on it still has to change, and a car
  changes to lanes with fewer: the cars for the off-ramp go to the outer
  lane on the way and into the ramp's lane when it comes. In the
  roundabout of two lanes a car turning right takes the outer lane, one
  turning left the inner, and those that go in or out from the inner lane
  cross the outer one: they give way to its cars, which stop for one that
  is crossing, and no car goes into a lane of the ring that holds a car
  every 12 m.
- `junctions.ent`: `town.ent` in a window: 1 to 7 choose the scene, up
  and down change how many cars come in, left and right how fast the time
  goes. Each car is in the colour of its speed against its road's limit,
  and the window says how many cars an hour get through. With 600 an hour
  on every street the lights (with two lanes each way) let some 2,100
  through, the roundabout of two lanes 2,150 and of one 1,750, the
  priority road 1,350 and the stops 850; the highway with 3,600 and 720
  from the ramp some 3,100, and the jam at the ramp goes back against the
  cars; with 3,600 and a quarter leaving, some 3,400.
- `show.ent`: `lanes.ent`'s roads in a window: Sugiyama's 22 cars, two
  lanes with trucks and three lanes too full to flow evenly, each car
  in the colour of how fast it goes against how fast its driver would.
  Under them, one road over the last 150 seconds, a line a second, read
  from the index `Traffic`: a jam is a red stripe that goes back
  against the cars. Space holds the cars, the arrows up and down make
  the time go faster or slower, 1 to 3 or a click choose the road, and
  B makes one of its cars brake hard.

```sh
tools/ent run --parallel examples/traffic/flow.ent > flow.csv
examples/traffic/check/flow.py
tools/ent run examples/traffic/sugiyama.ent
tools/ent run examples/traffic/show.ent
tools/ent run examples/traffic/junctions.ent
```

`test/Integration/traffic_ring.test` runs a few roads and checks the
flows without braking to the cell; `test/Integration/traffic_idm.test`
runs two roads of the driver model in ent and in C and compares them to
the bit, on one core and on all; `test/Integration/traffic_lanes.test`
does so for `lanes.ent` with one lane (against `check/idm.c`) and with
two and three (against `check/lanes.c`); `test/Integration/traffic_show.test`
and `traffic_junctions.test` compile `show.ent` and `junctions.ent`, and
`traffic_town.test` runs the four junctions of `town.ent` without a
window: no car runs into another, none of them locks up. `bench/traffic` times 120,000
cars on roads of three lanes.
