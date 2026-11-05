# 02 · The compiler owns the world

*29 September to 1 October 2026, the last three days under the name ecs-lang.
About 90 commits. Every feature came as a few small steps: the op and its
verifier, then its access, then its lowering, then an example, docs and a
benchmark entry.*

On day one the C host owned the memory. It allocated the columns and passed
them in, one argument per field. The toy's schedule took 15 arguments. These
three days moved the memory, and with it most of what a game or simulation
needs, into the compiler.

## Measure before you build: SoA stays

Before writing a layout pass, we wrote the frame by hand three ways: SoA, AoS
and AoSoA (blocks of 8 and 16). Then we measured.

- One M4 Max core streams about **130 GB/s, from L2 or from DRAM alike.** The
  frame ran at exactly that rate. That solved day one's mystery of the flat
  cost per entity from 1e5 to 1e7: the frame was bandwidth-bound at every
  size, cache or not.
- AoS was 1.7× slower. It drags unused fields through the cache.
- AoSoA didn't help. With up to five streams the bandwidth barely drops.

So there was no AoSoA pass, and SoA stayed. A layout pass nobody built is the
cheapest kind.

The surprise was **where columns start.** Large allocations come back page-
or 4 MB-aligned, so every column started at the same cache set. Moving each
column 17 cache lines further along made the single-threaded frame 7–8%
faster at every size, and lifted 16 streams from 8 to 65–84 GB/s.

The host couldn't be trusted to know that. A compiler can just do it, which
is the whole argument for the next step in one number.

## Capacities and one arena

`ecs.archetype @Body (...) capacity 100000` became mandatory. A hard upper
bound buys two things:

- **The world is laid out statically.** It's one arena: entity counts, then
  every column at a fixed offset, on a 64-byte boundary, 17 cache lines after
  the previous one. Creating a world zeroes only the counts. A big capacity
  costs address space, not memory, until rows are written.
- **The compiler knows from the size alone whether a loop can ever be worth
  running in parallel.** Archetypes below a threshold (1e6 then) always get
  the sequential loop, so small tables never pay for a fork.

`ecs-translate --ecs-to-c-header` generated the host's API from the same
layout: `ecs_world_create`, `ecs_Body_Position_x(world)` and one entry point
per schedule. The schedule went from 15 arguments to 2, and no host had to
know the argument order any more.

The arena made the fused frame **7–10% faster** (8.0% at 1e7). The C
reference, moved to the same arena, gained the same, and generated code still
matched it within 0.5%. The parallel threshold took the frame at 1e3 from
128 µs to 0.28 µs. Not forking turns out to be the best optimisation.

## Resources: things that exist once

`ecs.resource @Clock (dt: f32, frame: i64)` is state that isn't an entity.
(In ent-lang it's called a `unique`.) Systems may read a resource anywhere,
and a read inside a query is loaded once before the loop.

**Writing one inside a query is forbidden.** Every entity would write the
same field, so the result would depend on the order the entities ran in.
That rule keeps every query body entity-local, which is the property
everything parallel depends on. It also left a hole: you couldn't add up a
score. The hole stayed open for two days.

An archetype of capacity 1 holds at most one entity, so a query over it is an
`if`, not a loop. The player in the toy is that one entity.

## Optional components, and the day the compiler beat C

How do you store a component that entities gain and lose all the time, like a
stun? We measured four ways under churn before adding anything to the
language:

- **Archetype moves:** an entity moves to another table when it gains or
  loses the component.
- **A wide table with a presence byte**, written branch-free.
- **The same wide table with an `if`.**
- **A sparse set.**

No scheme won everywhere. Moves win below 0.1% churn per frame, the sparse set
at low density, and the wide table from 1% churn up, 3.1–3.5× faster than
moves at 10%. With an `if`, the wide table was 2–8× slower at medium
densities. Branches matter.

So `optional @Stunned` became a presence byte per entity. A query over it
computes a mask and stores `mask ? new : old`. That's legal here and not in
general C: every slot exists and belongs to its entity, so computing on an
absent entity's stale values and keeping the old ones changes nothing. Ops
that could misbehave on stale values, like division by a column, fall back to
an `if`.

Then the compiled program **beat the same wide table written in C: 9% per
system, 18% fused** (257 against 312 µs at 1e6 entities). We first blamed the
staggered columns. A `-DSTAGGER` build of the C ruled that out. The real
reason was the vectoriser: LLVM ran the compiled loop 16 entities wide and
the C one 4 wide. The compiled code compares the presence as a byte, while C
widens it to `int` first. Forcing the C to width 16 recovered all but 2–3%.
That the narrow type drives LLVM's choice is likely, not verified.

So "beats C" here means "beats the C I would have written". That's still the
point of a compiler.

## Spawn, despawn, and ids that survive

`ecs.spawn @Bullet(...)` appends a row and checks the capacity: overflowing
stops the program with "ecs.spawn exceeds the capacity of @A". `ecs.despawn`
is deferred. The query still visits every entity it would have, and when it
ends the listed rows are removed by swap-remove, last first.

Rows move, so entities needed **stable ids**: generational indices, an entity
table mapping slots to archetype and row, and a free list. With ids, `add` and
`remove` could finally *move* entities between archetypes. What `add` does is
decided by the storage, per archetype:

- set a presence byte where the component is optional;
- overwrite the fields where the archetype always has it;
- otherwise move the entity to the archetype that matches.

**Systems don't say which.** `status.mlir` and `status_moves.mlir` have the
same systems, byte for byte (the test diffs them), on two storages, and they
print the same.

Ids first cost 24 bytes per entity. A day later the compiler chose the id
scheme from what the program does:

- **Rows:** nothing despawns or moves, so an id is just archetype and row. It
  costs 0 bytes.
- **Slots:** entities move but never die, so there's no generation.
- **Generational:** entities are despawned. 32-bit ids when the capacities
  leave at least 8 generation bits, which is 10 bytes per entity at 1e6.

Ids got their own type, `!ecs.entity`, so arithmetic on them became
impossible, on purpose. A component field of that type is a relation: the
compiler now knew where ids were stored.

## Reading and writing other entities

`ecs.lookup %id @Position "x"` reads another entity's field and says whether
it was found. **A query may not look up a field it changes itself.** Which
entities saw the old value would depend on iteration order, and the verifier
points at the offending change. The homing-missile example keeps steering
toward its target ship until the ship leaves the arena. After that the lookup
says "not found" and the missile flies straight on, like a disappointed
missile.

`ecs.apply %id @Hull "hp" add %v` is the write side. Values are deferred to the
query's end and combined with `add`, `min` or `max` in **row order**. Each
query row writes its own buffer slot, so the loop stays parallel without
atomics, and the result is the same however many threads ran it. The test
checks that 1e8, 1 and −1e8 sum to 0. That's the row-order answer, and another
order would give 1.

Was determinism expensive? At 1e6 guns hitting random ships:

- **An atomic parallel add took 3.3× to 125× the time of the compiled apply**,
  depending on how many ships shared the hits. It lost to sequential C
  everywhere.
- The compiled apply was 16–40% slower than the same buffered scheme in C. Two
  fixes closed most of that gap:
  1. Load the id bounds once. LLVM can't hoist them out of a single arena by
     itself.
  2. **Drop a second barrier.** `--convert-scf-to-openmp` ends both the loop
     and the region with a barrier, where clang's `parallel for` waits once.
     The new `--ecs-omp-nowait` halved the fixed cost of a parallel frame,
     about 31 instead of 62 µs per region.

That second fix explained day one's other mystery: why our forks cost 65 µs
where an empty C region cost 35. Two "measured, not explained" items closed in
two days. It rarely goes that fast again.

## Reacting to change

`on [changed @Hull "hp"]` runs a query only for the entities whose hp changed
since it last ran. These are Entitas's reactive systems with the collector
built in, and they start the way Bevy's do: on the first run, everything
counts as added and changed. Every row carries stamps, meaning the tick of its
last event of each kind. A query's own events count on its next run, and the
compiler warns when a query triggers itself.

The change-detection benchmark had six schemes in C, written to resemble
Entitas, Bevy and Unity among others, against the compiled one:

- No scheme won at every rate.
- An Entitas-style collector was fastest up to 1% changed.
- Polling was fastest with everything changed, 3.5× on heavy work, because its
  loop vectorises.
- The compiled scan beat the same scheme in C by 6–19% on light work. Same
  story as before: the mask vectorises, C's `if` doesn't.

Scanning costs as much for one change as for a million. So every observed
stamp also got an **event log**: up to 64 ring-buffer segments, chosen by row
range so that parallel threads mostly append to their own segment. When a log
overflows, the query falls back to a scan. The first version just stopped
appending when full. A frame that changed every unit then walked only one
log's worth of them, and the checksums caught it. Walking the log was 22–45%
faster than scanning up to 1% changed. The segments walked in parallel made
parallel builds within 5% of scanning, or faster, at every rate.

## When does an effect become visible?

While writing the rules down, we found a bug. A query that spawned into an
archetype it would loop over *later* visited the new entities, but not ones
spawned into archetypes it had already looped over. So the result depended on
declaration order. The fix: a query counts every archetype it matches before
its first loop.

The rules went into `docs/sync-points.md`. **A query is the unit of
consistency:**

- it sees the world as it was when it started;
- it sees its own entity's changes at once;
- its applies, despawns and moves land at its end, in a fixed order.

Stages, fusion and parallel loops are used only where they give the same
result. That's why a frame's result doesn't depend on the number of threads.
Every later feature, including GPUs, partitions and indexes, had to fit this
page.

The score hole closed too. `ecs.accumulate @Score "points" add %v` is to
resources what `apply` is to other entities: deferred and combined in row
order.

## Less to write: inferred access and archetypes

Day one made systems declare `reads` and `writes`. But the analysis had been
deriving them per column all along, and the scheduler never used the
declarations. So access became **inferred**. Writing either list now declares
a contract, which the verifier still checks.

Archetypes went the same way. Capacities moved to components, spawns list
components, and the compiler infers one archetype per spawned set. Components
that some `add` or `remove` touches join it as optional. Inferred archetypes
never move entities, because the churn benchmark had found no storage that
wins everywhere, and presence is the safer default. The bullets example lost
its archetype declaration, and nothing else changed.

## What these three days settled

- **The compiler owns the memory**, and that's worth 7–10% before anything
  clever happens.
- **Storage is a decision of the program, not of its systems.** The same
  systems run on presence bytes or on moves.
- **Determinism is cheap when it's designed in.** Deferred writes in row order
  beat atomics by 3× to 125×.
- **Branch-free masks** are where generated code beats the C people write.
- **Write the semantics down**, because that's where the bugs hide.

The next day the language got its name and its syntax.

Next: [03 · From dialect to language](03-from-dialect-to-language.md)
