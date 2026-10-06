# Sync points: when effects become visible

This is the frame model of the language: which effects of a program are
visible to which other parts of it, and when. Everything else that needs a
notion of "now" — reactive queries, and later devices, shared-memory IPC and
indexes — is defined against it. Names follow the current dialect (`ent.`);
the language is to be called ent-lang.

The model in one sentence: **a query is the unit of consistency** — it sees
the world as it was when it started, its own entity's changes at once, and
everything it does to other entities or to the set of entities when it ends.

## Levels

| Level | What it is | Sync point at its end? |
|---|---|---|
| Entity step | One run of a query body for one entity | No |
| Query | One `ent.query`: its body for every entity it visits | **Yes: the commit point** |
| System | Its queries and system-level ops, in program order | Only through its queries' ends |
| Schedule | One call of a schedule: its runs, in program order | Yes: the frame boundary |
| Host | The program around the schedule calls: `main`, or a C host | — |

Stages, fusion and parallel loops are not levels: they are ways the compiler
may execute a schedule, allowed only where they give the same result as the
sequential order (see "Execution freedom").

## What a query sees and does

**Q1. Which entities a query visits.** The entities of every archetype it
matches that exist when the query starts, each exactly once. Entities spawned
while it runs are not visited by it; entities it despawns or moves are still
visited. A reactive query visits the subset of these with an event since it
last started (see R1).

**Q2. Its own entity, immediately.** `ent.get` and `ent.set` on the visited
entity read and write its row at once; a later op of the same body sees the
new value. Adding or removing a component the archetype holds optionally sets
or clears its presence at once, for the same reason: the row belongs to the
entity. No op of the body observes that, though: `ent.has` answers as of the
query's start (Q7).

**Q3. Other entities, as of the query's start.** `ent.lookup` reads another
entity's field. A query may not look up a field it changes itself (the
verifier rejects it), so every lookup sees the value the field had when the
query started, whichever entities have run before.

**Q4. Resources, as of the query's start.** No query writes a resource; it
may only accumulate into one (`ent.accumulate`), which takes effect at the
commit point. So a resource read inside a query sees the value it had when
the query started.

**Q5. Deferred to the commit point.** These do not take effect while the
query runs:

- `ent.apply`: values sent to other entities;
- `ent.accumulate`: values combined into a resource;
- `ent.despawn`;
- `ent.add` / `ent.remove` where the storage moves the entity to another
  archetype;
- `ent.connect` (inside a query) and `ent.disconnect`: the query visits the
  edges as they were when it started, in every edge loop.

**Q6. Immediate, but not visited.** `ent.spawn` appends the new entity at
once: its id is valid and it is visible to lookups and to every later query,
but not visited by the query that spawned it (Q1).

**Q8. Edges.** An edge loop (`ent.edges`) visits the edges the visited
entity had when the query started, in a fixed order (by source, then in the
order they were connected). Writing an edge's fields (`mut`) is immediate,
like writing the entity's own components: every edge is visited once per
query, by its source (`out`) or its target (`in`), and a query may not visit
a relation both ways when either loop writes the edges. An apply inside an
edge loop runs once per edge and is combined at the commit point like any
apply.

**Q7. Whether it has a component, as of the query's start.** `ent.has @C`
tells whether the visited entity had `C` when the query started; the query's
own `ent.add` and `ent.remove` do not change the answer. Where `C` is
optional the change is immediate (Q2) and where the entity moves it is
deferred (Q5), so answering with the current state would make the result
depend on how the archetypes store `C`. Filters (`with`, `without`, `any`)
are decided the same way: they choose the entities the query visits (Q1).

**Q9. Ancestors, as of the query's start, or in order.** A ref up a tree
(`!ent.ref<@C, up @R>`) reads a component of another entity, the nearest
ancestor that has it; which entity that is, is decided by the edges and
components as they were when the query started. Like a lookup (Q3) it must
not read a field the query writes. A query that cascades along the same
tree (`cascade @R`) may: it is defined as one query per depth of the tree,
run in order, first for the entities without a parent, then for their
children, and so on, each with its own commit point. So a ref up the tree
reads what this query wrote to the ancestor, which is at a smaller depth,
and never what it will write to an entity of the same depth or deeper.
`cascade @R leaves first` runs the depths the other way, the deepest first
and the entities without a parent last.

`ent.combine` through a `mut` ref up the tree is deferred to the commit
point of the depth that sent the value, where the values are combined in
the order the query visits the entities (fixed: by the edges, not by
rows). The ancestor is at another depth, so with `leaves first` it finds
them in its own field when its turn comes. The query may not read such a
field through a ref up the tree or look it up, so no entity sees what
others of its depth sent, and combining each value as its entity is
visited gives the same result. Nothing else in a cascading query is
deferred (Q5) so far, so one pass in an order that puts every parent
before its children, or after them, is that sequence of queries.

**Q10. Sorted trees.** The archetype holding the entities of a tree
declared `sorted` has its rows
put in the tree's order whenever the tree's edges are sorted, and that
also happens when the archetype gained or lost an entity: at the commit
point of a query that spawned into it or despawned from it (step 3), and
for a spawn outside a query before the system's next query and when the
system ends. No query sees rows move: it happens between queries. What the
order of rows decides (the order applies and accumulates are combined in,
the order a query with a `proc` visits entities in) therefore follows the
tree; ids, and everything read through them, do not change.

## The commit point: the end of a query

When a query has run for every entity it visits, in this order:

1. **Applies** are combined into their targets, by apply op (in program
   order), then source archetype, then source row (and for an apply in an
   edge loop, then the edge's position in the loop's order); then
   **accumulates** into their resources, in the same order; then edges
   **connected** in the query are added, by connect op, archetype and row. The order is fixed, so the result
   does not depend on how the query's loop ran, in parallel or not, even for
   floating-point `add`. Applies land before structural changes, while every
   id still leads to where its entity was.
2. **Structural changes** are applied per archetype: despawns free their ids,
   moves append their entities to the target archetype, and the rows they
   leave are filled by swap-remove. Within one query the last structural
   change to an entity wins.
3. **Relations** the query connected or disconnected are sorted again,
   which drops disconnected edges and edges whose source or target is no
   longer alive (despawned in step 2 or before).

After that, every later op — the next query of the same system, system-level
code, the next run — sees all of the query's effects.

## Systems and schedules

**S1.** A system runs its queries and system-level ops in program order;
each query's commit point precedes the next op.

**S2.** System-level code may write resources (`ent.write`), spawn and
connect edges; all take effect at once (a connect sorts its relation right
away).

**S3.** A schedule runs its systems in program order. One call of a schedule
is a frame: what the host sees after the call is the state after the last
run's last commit point.

**S4.** A run's condition (`run_if`) is evaluated when the run would start,
after every earlier run's commit points; a schedule's condition when the
schedule starts. A condition only reads resources (and the schedule's
parameters), so evaluating it has no effect; a run whose condition fails
does nothing.

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
  concurrently (`--ent-schedule`, `parallel-stages`).
- **Parallel entity loops**: a query whose body only touches its own entity
  (plus lookups and applies, which Q3 and Q5 make order-independent) may visit
  its entities in parallel.
- **Fusion**: consecutive systems may be run per entity rather than per
  query, which is equivalent while every body only touches its own entity. It
  stops at systems that write resources, change which entities archetypes
  hold, look up or apply to other entities, read up a tree or cascade along
  one, or react to events: those depend on other queries having reached
  their commit points.
- **Cascading queries**: the entities of one depth are independent of each
  other (Q9) and could be visited in parallel; the compiler visits them one
  after another so far.

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

**H3.** Edges connected through the header (`ent_<R>_connect`) are sorted
when the next schedule call starts. A despawn leaves the edges of the
despawned entity in place until the relation is next sorted; until then
loops skip them (their end is no longer the visited entity) and applies
along them are dropped (their target is dead).

**H4.** A relation that names its ends' components (`from C to D`) relies
on them: the header's connect refuses an end without its component, and
the host must not clear the presence of an end's component through the
header while the entity has edges.

## Extern systems and `main`

An extern system is a run like any other: it sees what the runs before it
committed, and what it does to the world through the header is there when
it returns. `main` only calls schedules, so everything it reads (a loop's
condition, a call's arguments) is read at a frame boundary.

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
