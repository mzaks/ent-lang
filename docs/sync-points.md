# Sync points: when effects become visible

This is the frame model of the language: which effects of a program are
visible to which other parts of it, and when. Everything else that needs a
notion of "now" — reactive queries, and later devices, shared-memory IPC and
indexes — is defined against it. Names follow the current dialect (`ecs.`);
the language is to be called ent-lang.

The model in one sentence: **a query is the unit of consistency** — it sees
the world as it was when it started, its own entity's changes at once, and
everything it does to other entities or to the set of entities when it ends.

## Levels

| Level | What it is | Sync point at its end? |
|---|---|---|
| Entity step | One run of a query body for one entity | No |
| Query | One `ecs.query`: its body for every entity it visits | **Yes: the commit point** |
| System | Its queries and system-level ops, in program order | Only through its queries' ends |
| Schedule | One call of a schedule: its runs, in program order | Yes: the frame boundary |
| Host | The program around the schedule calls | — |

Stages, fusion and parallel loops are not levels: they are ways the compiler
may execute a schedule, allowed only where they give the same result as the
sequential order (see "Execution freedom").

## What a query sees and does

**Q1. Which entities a query visits.** The entities of every archetype it
matches that exist when the query starts, each exactly once. Entities spawned
while it runs are not visited by it; entities it despawns or moves are still
visited. A reactive query visits the subset of these with an event since it
last started (see R1).

**Q2. Its own entity, immediately.** `ecs.get` and `ecs.set` on the visited
entity read and write its row at once; a later op of the same body sees the
new value. Adding or removing a component the archetype holds optionally sets
or clears its presence at once, for the same reason: the row belongs to the
entity.

**Q3. Other entities, as of the query's start.** `ecs.lookup` reads another
entity's field. A query may not look up a field it changes itself (the
verifier rejects it), so every lookup sees the value the field had when the
query started, whichever entities have run before.

**Q4. Resources, as of the query's start.** No query writes a resource; it
may only accumulate into one (`ecs.accumulate`), which takes effect at the
commit point. So a resource read inside a query sees the value it had when
the query started.

**Q5. Deferred to the commit point.** These do not take effect while the
query runs:

- `ecs.apply`: values sent to other entities;
- `ecs.accumulate`: values combined into a resource;
- `ecs.despawn`;
- `ecs.add` / `ecs.remove` where the storage moves the entity to another
  archetype.

**Q6. Immediate, but not visited.** `ecs.spawn` appends the new entity at
once: its id is valid and it is visible to lookups and to every later query,
but not visited by the query that spawned it (Q1).

## The commit point: the end of a query

When a query has run for every entity it visits, in this order:

1. **Applies** are combined into their targets, by apply op (in program
   order), then source archetype, then source row; then **accumulates** into
   their resources, in the same order. The order is fixed, so the result
   does not depend on how the query's loop ran, in parallel or not, even for
   floating-point `add`. Applies land before structural changes, while every
   id still leads to where its entity was.
2. **Structural changes** are applied per archetype: despawns free their ids,
   moves append their entities to the target archetype, and the rows they
   leave are filled by swap-remove. Within one query the last structural
   change to an entity wins.

After that, every later op — the next query of the same system, system-level
code, the next run — sees all of the query's effects.

## Systems and schedules

**S1.** A system runs its queries and system-level ops in program order;
each query's commit point precedes the next op.

**S2.** System-level code may write resources (`ecs.write`) and spawn; both
take effect at once.

**S3.** A schedule runs its systems in program order. One call of a schedule
is a frame: what the host sees after the call is the state after the last
run's last commit point.

## Reactive queries

**R1.** A reactive query (`on [...]`) collects the entities that had one of
its trigger events since it last *started* and that it would visit now (Q1).
Events are recorded when they happen (stamps, event logs), but a query starts
by closing its window: events caused after that — its own included — count on
its next run. On its first run every existing entity counts as added and
changed.

**R2.** An event is "visible" to a reactive query once the query causing it
has reached its commit point; since reactive queries never run concurrently
with the systems causing their events (the scheduler orders them through the
tick counter), this is the same as "before the reactive query starts".

## Execution freedom

The compiler may execute a schedule differently from its program order only
where the result is the same:

- **Stages**: runs whose column accesses do not conflict may run
  concurrently (`--ecs-schedule`, `parallel-stages`).
- **Parallel entity loops**: a query whose body only touches its own entity
  (plus lookups and applies, which Q3 and Q5 make order-independent) may visit
  its entities in parallel.
- **Fusion**: consecutive systems may be run per entity rather than per
  query, which is equivalent while every body only touches its own entity. It
  stops at systems that write resources, change which entities archetypes
  hold, look up or apply to other entities, or react to events: those depend
  on other queries having reached their commit points.

**Determinism.** Given the same world and arguments, a frame's result does
not depend on these choices or on the number of threads: bodies cannot
observe iteration order (Q2, Q3), and everything that combines values across
entities does so in a fixed order (commit point, step 1). The integration
tests build every example sequentially, fused, with parallel stages and with
parallel entity loops, and compare outputs.

## The host

**H1.** Between schedule calls the host may read and write the world through
the generated header. During a call it must not touch it: stages and parallel
loops assume nothing else writes the arena.

**H2.** Spawns through the header are events (reactive queries see them);
writes through the header's column accessors are not tracked.

## Defined against this model later

These are proposals from the ent-lang design notes, not built:

- **Devices** (side effects as request and response entities): run at
  schedule level, between runs, so a device consumes the requests committed
  by earlier queries and its responses are visible to later runs, like any
  system's effects.
- **Shared-memory IPC**: ownership of shared columns changes hands only
  between runs (an epoch per handoff), never inside a query.
- **Indexes**: maintained at commit points, so an index is consistent at
  every query's start and, within a query, reflects the state at its start,
  like lookups (Q3).
- **Transient events**: despawned at the frame boundary unless consumed.
