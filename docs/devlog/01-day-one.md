# 01 · Day one: an ECS as an MLIR dialect

*28 September 2026. 19 commits, the first at 18:17 and the last at 19:42.
Those are commit times, not working hours: the prototyping before it took
longer than an hour and a half.*

## The plan, as the first README stated it

- **M0:** a dialect that parses, prints and verifies.
- **M1:** lower a toy simulation to loops and run it.
- **M2:** access analysis and a scheduling pass, for parallelism the
  compiler can prove.
- **M3:** layout choice, fusing systems, benchmarks.

M0 to M2 and half of M3 were done that evening. That says less about speed
than about how small each milestone was. Keeping steps small became the
habit that carried the project.

## The toy

Every compiler project needs a toy, and ours was gravity:

```mlir
ecs.component @Position (x: f32, y: f32)
ecs.component @Velocity (dx: f32, dy: f32)
ecs.component @Mass (kg: f32)

ecs.system @gravity(%dt: f32, %g: f32) reads [@Mass] writes [@Velocity] {
  ecs.query (%v: !ecs.ref<@Velocity, mut>, %m: !ecs.ref<@Mass>) {
    %dy = ecs.get %v "dy" : !ecs.ref<@Velocity, mut> -> f32
    %dv = arith.mulf %g, %dt : f32
    %ndy = arith.subf %dy, %dv : f32
    ecs.set %v "dy", %ndy : !ecs.ref<@Velocity, mut>, f32
  }
}

ecs.schedule @frame(%dt: f32) {
  %g = arith.constant 9.81 : f32
  ecs.run @gravity(%dt, %g) : f32, f32
  ecs.run @integrate(%dt) : f32
}
```

The `%m` binding is there only so the query skips anything without mass.
Today the same system reads:

```
gpu system gravity(dt: f32, g: f32) {
  for v: mut Velocity with Mass {
    v.dy -= g * dt
  }
}
```

It's the same idea in a fraction of the characters, and it runs on a GPU too.
The syntax caught up later.

## The one guarantee that matters

The dialect has five ops:

- `component`: fields, which must be scalars;
- `system`: parameters, plus `reads` and `writes` sets;
- `query`: one `ref` per component;
- `get` and `set`;
- `schedule` with `run` steps.

The verifier checks one central promise: **every access a system makes is
covered by what it declared.** A `mut` ref needs the component in `writes`.
A `set` needs a `mut` ref. A system cannot get a ref any other way than from a
query, so nothing can sneak around the declared sets.

It sounds bureaucratic. It's also what everything later relies on. Scheduling,
fusion, parallel loops, GPU offload and partitions are all just consequences
of knowing who touches what. (Three days later access became *inferred* rather
than declared, but the promise stayed.)

## Archetypes: a closed world

The second idea of the day was `ecs.archetype @Body (@Position, @Velocity,
@Mass)`. An archetype is a table of entities that share a set of components,
and **the set of archetypes is closed and known at compile time.** So matching
a query to its tables happens in the compiler, not at run time. A query that
matches no archetype can never run, so it's deleted with a warning.

The toy got three archetypes:

- bodies with mass, which gravity pulls;
- massless particles, which gravity skips;
- scenery, which only has a position and that nobody touches. Scenery is the
  control group.

## M1: from dialect to a number on screen

`--ecs-lower-to-loops` turned each query into one `scf.for` per matching
archetype, and each `get` and `set` into a load or store at the loop index.
The world went in as plain arguments: per archetype, a count and one column
per field.

A C host filled the columns, called the lowered `frame` three times and printed
the entities. Gravity lowers `dy` by 4.905 per frame. Particles drift, and
scenery sits there. I checked the expected values by hand, which is the most
satisfying way to verify that 9.81 × 0.5 = 4.905.

## M2: parallel stages, and why they lost

The access analysis worked per column, not per component. From it,
`--ecs-schedule` grouped the `run` steps into stages: runs whose columns don't
conflict go in the same stage, which lowers to OpenMP sections. The toy got
wind and decay systems so there was something to run side by side.

It worked, and it was the wrong kind of parallelism. More about that below.

## M3, first half: fusion and parallel entities

Two more lowerings came the same evening:

- **Parallel entities:** a query that only touches its own entity becomes
  `scf.parallel`, which becomes an OpenMP loop.
- **Fusion:** all the systems of a schedule become one loop per archetype,
  where the access sets allow it. Gravity and integrate then share a pass over
  the bodies.

## The first benchmark

The setup was `bench/run.py` on an M4 Max. Every number is a median over five
processes, each the best of three repetitions. All variants had to produce
bit-identical checksums, or the run didn't count.

The machine wasn't idle: macOS storage services and a Zoom call were running.
The results file says so. That became a rule: a number comes with the
conditions it was taken under.

Nanoseconds per frame, with the OpenMP runtime defaults:

| variant | n=1e4 | n=1e6 | n=1e7 |
|---|---|---|---|
| loops | 5,946 | 515,071 | 5,216,323 |
| fused | 5,059 | 497,226 | 4,965,371 |
| hand-written C, fused | 5,055 | 495,850 | 4,974,483 |
| stages in parallel | 68,857 | 476,082 | 4,438,231 |
| fused, entities in parallel | 130,190 | 217,465 | 1,858,796 |

What held:

- **The generated fused code matched hand-written C within 1% at every
  size.** Adding `restrict` to the C changed nothing measurable.
- Fusion saved 15% at n=1e4 and 3–5% elsewhere. Small, but it reproduced.
- Entity parallelism needs about a million entities to pay off. Above that it
  gave 2.3× at 1e6 and 2.7× at 1e7.
- **Parallel stages weren't worth it.** They gave 15% at best and were far
  slower at small sizes. Splitting *data* won over splitting *systems*
  wherever parallelism paid at all. That is the core ECS bet, confirmed on day
  one.

The results also had a section titled **"Measured, not explained"**. Every fork
cost about 65 µs, where an empty `#pragma omp parallel` cost 35. The cost per
entity stayed flat from 1e5 to 1e7, although only 1e5 fits in cache. Nobody
knew why yet, and the file said so instead of guessing.

That section header stuck. A benchmark that is honest about what it doesn't
explain is worth more than a confident one, and it's also cheaper. A wrong
explanation will send you, three weeks later, optimising the wrong thing.

## What day one settled

- The **semantics come first**, the spelling later.
- **Know who touches what**, and let the compiler do the rest.
- **Small steps**, each with a test, and a number where a number makes sense.
- **Data parallelism over task parallelism**, measured rather than believed.
- **Compare with C, honestly**, and write down what you can't explain.

Next: [02 · The compiler owns the world](02-the-compiler-owns-the-world.md),
where the C host stops owning the memory.
