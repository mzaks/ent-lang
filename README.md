# ent-lang

A research language built on the Entity Component System pattern; the name
reads as *Ent*-ity plus *lang*, and as German "entlang" ("along"): systems
run along the entities their queries match. The semantics are developed
first as an MLIR dialect (`ent`), written by hand and transformed by passes;
a surface syntax comes later. (Until 2026-10-01 it was called ecs-lang, with
an `ecs` dialect and `ecs_` names in generated C; `bench/RESULTS.md` keeps
the old names for results taken before.)

## Build

Requires Homebrew LLVM (tested with 22.1.8, which ships MLIR, FileCheck and
`mlir-runner`) and CMake. `lit` lives in a local venv.

```sh
python3 -m venv .venv && .venv/bin/pip install lit
cmake -S . -B build -DMLIR_DIR=/opt/homebrew/opt/llvm/lib/cmake/mlir \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
cmake --build build --target check-ent
build/bin/ent-opt examples/integrate.mlir
```

Run the toy simulation: generate the world's C header, lower the program
with fused, parallel entity loops, and link it with its C host and the
OpenMP runtime. Drop `parallel-entities`, `--convert-scf-to-openmp` and the
OpenMP flags for a sequential build.

```sh
LLVM=/opt/homebrew/opt/llvm
build/bin/ent-translate --ent-to-c-header examples/integrate.mlir \
    -o /tmp/integrate_world.h
build/bin/ent-opt examples/integrate.mlir \
    "--ent-lower-to-loops=fuse-systems=1 parallel-entities=1" --symbol-dce \
    --convert-scf-to-openmp --canonicalize --ent-omp-nowait \
    --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts \
  | $LLVM/bin/mlir-translate --mlir-to-llvmir -o /tmp/integrate.ll
$LLVM/bin/clang -O2 -Wno-override-module -I/tmp /tmp/integrate.ll \
    examples/host/integrate_main.c -L$LLVM/lib -lomp -Wl,-rpath,$LLVM/lib \
    -o /tmp/integrate && /tmp/integrate
```

Inspect the analysis behind the schedule:

```sh
build/bin/ent-opt examples/integrate.mlir --ent-print-access -o /dev/null
build/bin/ent-opt examples/integrate.mlir --ent-schedule=explain=1
```

## Writing ent-lang

Programs are written in `.ent` files and imported into the dialect below
with `ent-translate --import-ent`; the syntax is described in
[`docs/syntax.md`](docs/syntax.md), and `examples/*.ent` are the examples
written that way (the integration tests run both forms and expect the same
output; `examples/filters.ent`, for filters, `has` and `run_if`, and
`examples/snn*.ent`, for relations, exist only in ent-lang):

```
component Position { x: f32 }
component Velocity { dx: f32 } capacity 64

system fly(dt: f32) {
  for p: mut Position, v: Velocity {
    p.x += v.dx * dt
  }
}

schedule frame(dt: f32) { fly(dt) }
```

```sh
build/bin/ent-translate --import-ent examples/bullets.ent -o /tmp/bullets.mlir
build/bin/ent-translate --ent-to-c-header /tmp/bullets.mlir \
    -o /tmp/bullets_world.h
build/bin/ent-opt /tmp/bullets.mlir --ent-lower-to-loops ...  # as below
```

## The dialect today

When each effect becomes visible to the rest of the program (the frame
model: queries as the unit of consistency, their end as the commit point)
is specified in [`docs/sync-points.md`](docs/sync-points.md).

- `ent.component @Position (x: f32, y: f32) capacity 100000`: scalar fields,
  abstract layout; the optional capacity bounds how many entities can have
  the component.
- `%id = ent.spawn (@Position, @Velocity)(%x, %y, %dx, %dy) : f32, f32, f32,
  f32` creates an entity with these components, a value for every field in
  order, and returns its id. Archetypes need not be declared: the compiler
  (`--ent-infer-archetypes`, which every other pass and `ent-translate` run
  first) gives every set of spawned components an archetype, named after
  them (`@Position_Velocity`, with `_archetype` appended if the name is
  taken). A component some `ent.add` can give these entities, or some
  `ent.remove` can take away, is stored in it as optional (below), so
  entities never move between inferred archetypes; a spawn still starts
  with every component it lists. Its capacity is the smallest among the
  components that stay required, or the module's `ent.default_capacity`;
  without either it is an error. Shapes only the host spawns (through the
  generated header) are invisible to the compiler and need a declared
  archetype. `examples/bullets.mlir` spawns bullets this way.
- `ent.archetype @Body (@Position, @Velocity, @Mass) capacity 100000`: a
  table that entities are stored in, with a hard upper bound on its size.
  Declaring one takes full control: its name, capacity and storage (moves
  between archetypes, below). A spawn whose components are exactly a
  declared archetype's required ones lives there.
  The set of archetypes is closed, so every query is matched to its
  archetypes at compile time. An archetype of capacity 1 holds at most one
  entity (a player, a camera); its queries become a guard instead of a
  loop.
- `ent.archetype @Character (@Position, optional @Stunned) capacity N`: an
  optional component may be present or absent per entity. It is stored as
  its field columns plus a presence byte, so `ent.add @Stunned(%t) : f32`
  and `ent.remove @Stunned`, inside a query, write the entity's own row
  instead of moving it to another archetype. Queries that bind it run only
  for the entities that have it.
- `%id = ent.spawn @Body(%x, %dx, %m) : f32, f32, f32` spawns into a declared
  archetype, with a value for every field of its non-optional components
  (optional components start absent). Either way the new entity exists at
  once (its id is valid, lookups find it), but the query that spawned it
  does not visit it. Spawning beyond the
  capacity stops the program with a message. `ent.despawn`, inside a query,
  removes the entity it visits; the removal is deferred to the end of the
  query, so the query still visits every entity it would have. A system that
  does either and declares its access (see `ent.system`) lists in `writes`
  the components it spawns, or the declared archetype (`writes [@Body]`); to
  despawn, the components its query binds, or the declared archetype.
- `ent.add` and `ent.remove` work on any component; the archetypes decide
  what they do. Where the component is optional they set or clear its
  presence; where it is not, the entity moves to the archetype with exactly
  the resulting components (which must exist; the verifier says so
  otherwise), deferred to the end of the query like a despawn. So choosing
  between an optional component and separate archetypes is a storage
  decision: `examples/status.mlir` and `examples/status_moves.mlir` run the
  same systems both ways and print the same result.
- `%id = ent.entity`, inside a query, is the visited entity's id, of type
  `!ent.entity`. Ids stay valid while an entity moves between rows and
  archetypes and stop being alive when it is despawned. How many bits an id
  takes is up to the compiler (see Entity ids below), not the program.
- `%x, %found = ent.lookup %id @Position "x" : f32` reads a field of another
  entity, whichever archetype it lives in; `%found` is false (and the value 0)
  if the id is dead or the entity lacks the component. A component field of
  type `!ent.entity` (`@Target (entity: !ent.entity)`) is how entities refer
  to each other; `examples/homing.mlir` has missiles steering towards their
  target ship. Lookups only read. A query may not look up a field it changes
  itself (the verifier rejects it: which entities saw the old value would
  depend on iteration order), and a system with lookups is not fused with
  others for the same reason; under those rules a query with lookups still
  runs in parallel.
- `ent.apply %id @Hull "hp" add %damage : f32` writes to another entity: it
  combines a value into a field of the entity `%id`, with the rule `add`,
  `min` or `max` (signed for integers). Applies are deferred to the end of
  the query, where every target's field becomes `field ⊕ v1 ⊕ v2 ⊕ ...`
  over the values sent to it; so the query itself sees the old values and
  may read (`ent.get`, `ent.lookup`) or `ent.set` the field, and the
  applies land on top. Values are combined in a fixed order (by apply, then
  archetype, then row), so the result does not depend on how the query ran,
  in parallel or not, even for floating-point `add`. A value sent to a dead
  id or to an entity without the component is dropped. An apply sits in a
  query, not inside a loop there, and all applies of one query to a field
  use the same rule; a system that declares its access lists the component
  in `writes`.
  `examples/damage.mlir` has torpedoes damaging their target ships.
- `ent.relation @Synapse (weight: f32) capacity 1000000` declares edges
  between entities, with data: each goes from a source to a target and
  carries the fields. Edges are not entities; an entity may have any number
  of them. Inside a query, `ent.edges @Synapse out (%s: !ent.ref<@Synapse>,
  %post: !ent.entity) { ... }` visits the edges of the visited entity (`in`:
  those to it), `%post` being the other end; `ent.get`/`ent.set` read and,
  with `mut`, write the edge's fields (every edge is visited once per query,
  so the write is local). An `ent.apply` inside runs once per edge, combined
  at the query's end by apply, archetype, row and edge, so pushing values
  along edges is deterministic in parallel too. `ent.connect @Synapse %a,
  %b (%w)` adds an edge (outside queries at once, inside at the query's
  end), `ent.disconnect` inside an edge loop removes the visited one at the
  query's end. `examples/snn.ent` and `examples/snn_pull.ent` are a spiking
  neural network pushing spikes along outgoing synapses and gathering them
  along incoming ones; both agree with a plain C simulation to the bit.
- `ent.query (%b: !ent.ref<@Bar, mut>) on [changed @Hull "hp", added @Hull,
  removed @Shield] { ... }` is a reactive query, after Entitas's reactive
  systems: it runs only for the entities that had one of these events since
  it last started, and that match the query now. `added @C` fires when an
  entity gains `C` (`ent.add`, or spawned with it), `removed @C` when it
  loses `C` and stays alive, `changed @C "f"` on every write to the field
  (`ent.set`, `ent.apply`, `ent.add`, spawning), even of the same value, and
  `changed @C` on a write to any of its fields. On its first run every
  existing entity counts as added and changed. Events a query causes itself
  count on its next run; the compiler warns where it can prove that, since
  such a system usually wants to run every frame, be split, or react to a
  marker component. It also warns about triggers nothing in the program
  can fire. A query cannot react to `removed @C` and bind `C`. Writes the
  host makes through the header's column accessors are not tracked;
  spawning through the header is. `examples/reactive.mlir` redraws health
  bars only for ships that were hit. A trigger may set the capacity of its
  event log (below): `changed @Hull "hp" log 4096`, or `log 0` for none.
- `ent.resource @Clock (dt: f32, frame: i64)`: world state that exists
  exactly once and is not an entity. Systems access it with
  `ent.read @Clock "dt" : f32` and
  `ent.write @Clock "frame", %v : i64`. Reads are allowed anywhere in a
  system; writes are not allowed inside a query, where every entity would
  write the same field and so depend on the others. Inside a query,
  `ent.accumulate @Score "points" add %bonus : i64` combines values into a
  resource field instead (`add`, `min` or `max`), with the same rules and
  guarantees as `ent.apply`: combined when the query ends, in a fixed
  order, so the result is the same however the query ran.
- `ent.system @s(%params) { ... }`: what a system reads and writes is
  inferred from its body (`--ent-print-access` shows it). It may also declare
  it, `ent.system @s(%params) reads [...] writes [...] { ... }`, as a
  contract: a system that declares either list (even `reads []`) must stay
  within both, and the verifier says where it does not. `writes` implies
  read.
- `ent.query (%p: !ent.ref<@Position, mut>, ...) { ... }`: body runs once per
  matching entity; refs exist only as query arguments. Filters follow the
  arguments: `with [@Enemy] without [@Shield] any [@Fire, @Ice]` (the
  entity has every `with` component, no `without` one, and one of each `any`
  group). A query binds or filters by at least one component. Where an
  archetype always or never holds a filtered component the match is decided
  for the archetype; where it holds it optionally, by the presence per
  entity, which joins the mask of optional bindings.
- `%b = ent.has @Shield`: whether the visited entity had the component when
  the query started (the query's own adds and removes land at its end, as
  far as `has` can tell, however the component is stored); a constant where
  the archetype always or never holds it.
- `ent.get` / `ent.set`: field access through a ref; `set` needs `mut`.
- `ent.schedule @frame(%params) { ent.run @s(...) }`: program order is the
  semantic order. `ent.run @s() if { ...; ent.yield %c : i1 }` runs the
  system only if the condition holds when the run would start;
  `ent.schedule @frame(%p: T) if { ^bb0(%p: T): ... } { ... }` runs the
  whole frame under a condition. Conditions read resources and compute
  with ops free of side effects.
- `ent.stage { ent.run ... }`: runs that commute and may execute in
  parallel; stages execute in order.

The verifier checks that every query and every resource access stays
inside its system's declared access, where it declares one, that no query
binds a component twice or writes a resource, that refs are only used by
`ent.get` and `ent.set`, and that field names and types match the
declarations.

## Access analysis and scheduling

A system's access is computed per column, `Archetype.Component.field`, from
the `ent.get` and `ent.set` ops it contains, in every archetype its queries
match; it does not need declarations, and where a system declares its
access the analysis is still finer: two systems that both write `Velocity`
do not conflict if one only touches `Body.Velocity.dy` and the other only
`Particle.Velocity.dx`, and binding a component to select archetypes is not
a read; nor is a filter, which reads at most the presence where the
component is optional. A run's condition adds its resource reads to the
run's access. Resource fields are columns too (`Clock.frame`), so a system that
writes a resource is ordered against the systems that read it, and so is
the presence of an optional component (`Character.Stunned?`): adding or
removing it is ordered against every query that binds it. Every query reads
its archetypes' entity counts (`Bullet.count`), and a spawn or despawn
writes the count and every column of the archetype, so structural changes
are ordered against every system that touches the archetype. A lookup reads
the entity table (`entities`) and the field in every archetype holding the
component; structural changes write the entity table, so a despawn waits
for the lookups before it. An apply reads the entity table too and writes
the field in every archetype holding the component, so the systems that
read that field wait for it. Reactive queries read the stamps of their
triggers (`A.C.f@`, `A.C@`, `A.C+`, `A.C-`), which the ops causing those
events write, and advance a tick counter (`ticks`) that those ops read:
a reactive system never shares a stage with a system causing observed
events. Any other op
with memory effects (a call, say) makes a system opaque, and opaque systems
conflict with everything.

`--ent-schedule` puts each run into the earliest stage after every earlier
run it conflicts with. In the example, gravity, wind and decay share the
first stage and integrate follows. Ops with effects in a schedule body act
as barriers.

## Lowering

`--ent-lower-to-loops` turns systems into private functions and schedules into
public ones with a C interface (`_mlir_ciface_<schedule>`). Each query becomes
one `scf.for` per matching archetype, and field access becomes `memref.load`
and `memref.store` on that archetype's columns. Stages dissolve into calls,
or with `parallel-stages=1` a stage of several runs becomes an `omp.parallel`
region with one `omp.section` per run.

## World storage

The language owns the world's storage. From the archetypes' components and
capacities and from the resources, the compiler lays out the whole world as
one arena: an i64 entity count per archetype, then each resource on its own
cache line, then one column per field at a fixed offset, each archetype's
id column, one buffer per `ent.apply` and archetype its query matches (a
target id and a value per row), each relation's edges, and the entity table
that maps an id to its archetype and row. Programs with reactive queries also keep a tick counter
and each reactive query's last tick in the header, and stamp columns after
the component columns of the archetypes that need them.
Every column starts on a 64-byte boundary, 17 cache lines past the end of
the previous one; columns packed from a page-aligned base would start at
the same cache set, which cost a single core 7-8% (see
`bench/RESULTS.md`). Lowered functions take the arena after their own
parameters and read columns through statically shaped views.

A relation is a table of source ids, target ids and a column per field,
kept sorted by source (compressed sparse rows): an offset per entity key
(its slot, or for row ids the id itself) says where its edges start, so an
`out` loop reads one contiguous range. A relation the program visits only
by incoming edges is sorted by target instead (first by source, then
stably by target, so the visiting order stays the same), and an `in` loop
reads its range in order; where the program visits edges both ways,
offsets by target and the table positions of those edges follow. Fields
of the visited entity that an edge loop sets are carried through the loop
as values and stored once after it: nothing else in the loop can reach
them, which LLVM cannot tell from the arena's views (pull 6.3-9.4x slower
than hand-written C before, 1.09-1.27x after; see `bench/RESULTS.md`).
Connects append and mark the relation unclean; a sort (a stable counting
sort through scratch columns, O(edges + keys)) runs where edges changed:
at a schedule's start for edges the host connected, after a system-level
connect, and at the end of a query that connected or disconnected. It
drops dead edges and edges to entities no longer alive. With generational
ids a slot may be reused before the next sort, so a loop only counts edges
whose own end is the visited entity. An apply inside an edge loop has, per
row, a flag saying whether the row ran the loop, and per edge a target and
a value; the end of the query walks the rows that ran and their edges in
order.

`ent-translate --ent-to-c-header` emits the C API for hosts:
`ent_world_create`/`ent_world_destroy`; per archetype a capacity, a count,
`ent_Body_spawn(world)` (returns the new id, or `ENT_NO_ENTITY` if the
archetype is full) and `ent_Body_spawn_n(world, n)`, the id column, and typed
column accessors such as `ent_Body_Position_x(world)`; entity lookups
`ent_entity_alive`, `ent_entity_archetype` and `ent_entity_row`; one accessor
per resource field such as `ent_Clock_frame(world)`; and one entry point per
schedule, such as `ent_frame(world, dt)`. The header allocates ids exactly as
the lowered program does. Creating a world zeroes only the counts, resources
and entity counters, so capacity costs address space, not memory, until
columns are written. Parallel stages and loops assume nothing else writes the
arena while a schedule runs.

## Entity ids

The compiler picks how ids are represented from the capacities and from
which structural changes the program makes:

- **Rows**: nothing is despawned or moved, so an entity keeps its row and
  its id is `archetype << rowBits | row`. No id column, no entity table.
- **Slots**: entities move but are never despawned, so slots are never
  reused; an id is a slot of an entity table that holds only locations.
- **Generational**: entities are despawned and slots reused; an id is
  `generation << slotBits | slot`, and the table holds a generation per
  slot besides the location. Freed slots are chained through their
  locations, so the free list costs nothing extra.

Locations pack `archetype << rowBits | row` into 32 bits where they fit. An
id is 32 bits wide whenever that leaves at least 8 generation bits (so at up
to 2^24 entities); otherwise it is 64 bits. A generation wraps around at its
bit width: an id kept across that many reuses of its slot could come alive
again. The module attributes `ent.entity_id_bits = 64` and
`ent.min_generation_bits = N` trade memory for more margin. At 10^6
entities, generational ids cost 10 bytes per entity (id 4, generation 2,
location 4) instead of 24; rows ids cost nothing.

`!ent.entity` lowers to the chosen integer; the generated header's
`ent_entity` type matches it.

## Fusion and entity parallelism

Every access in a query goes through a ref to the current entity, and
different archetypes share no columns. Two consequences the lowering uses:

- `fuse-systems=1` inlines the systems of a schedule and emits one loop per
  archetype holding every matching query body in program order. This is
  legal for any sequence of systems without other effects, conflicting or
  not: running all bodies for one entity before the next gives the same
  result as running each query to completion. A system that writes a
  resource ends a fused sequence, because fusion moves system-level code
  ahead of the queries. So does a run under a condition, which runs as a
  whole or not at all, and a system with edge loops or connects.
- Since no query writes a resource, a resource read inside a query is
  loaded once before the loop.
- A spawn checks the capacity, writes row `count`, allocates an id (the
  last freed slot, or the next unused one) and bumps the count. A despawn
  or move appends the row to a pending list; when the whole query has run,
  the listed rows are applied last first: a despawn frees the id (bumping
  its generation), a move appends the entity to the target archetype, and
  either way the archetype's last row moves into the hole (swap-remove),
  with the moved entity's location updated in the entity table. Within one
  query, the last structural change to an entity wins. Queries with
  structural changes stay sequential (the lists are shared), and a system
  with any ends a fused sequence.
- An apply fills the entity's row of a buffer, a target id and a value
  (or the all-ones id, "no target", where it did not run or the entity is
  masked out), so the query loop stays entity-local and may run in
  parallel. When the query has run, one sequential loop per apply and
  archetype goes over the buffer in row order, finds each target like a
  lookup, and combines the value into its field (the counts and slots that
  ids are checked against are loaded once, before the loop, since
  combining cannot change them); this happens before the
  query's despawns and moves, while every id still leads to its entity. A
  system with applies ends a fused sequence: a later system reading the
  field must see the combined values.
- Reactive queries find their entities by version stamps: an i64 tick per
  row for each event some reactive query observes, stored only in the
  archetypes whose entities such a query can see (directly, or after
  moves). An op causing an observed event stores the current tick next to
  it; spawns stamp their row; moves carry stamps along, or stamp the move
  itself where it is the event. A reactive query starts by taking the tick
  it last started at and advancing the counter, then runs for every row and
  keeps its effects only where a stamp is newer, branch-free like a body
  masked by an optional component. Reactive systems end a fused sequence,
  since they advance the counter before their loop.
- Besides the stamps, every observed event has an event log: the entities
  whose stamp moved on to a new tick, so that a reactive query can visit
  just those instead of scanning every row (`bench/reactive/`: no single
  scheme wins at every change rate). Its capacity is an eighth of the
  entities that can carry the stamp unless a trigger asks for another
  (`log N`), and it is split into up to 64 segments, each a ring with its
  own count on a cache line of its own. An event at row r of n entities
  goes to segment r * segments / n, so the contiguous row ranges of a
  parallel loop's threads mostly append to segments of their own (an
  atomic add keeps the shared ones correct); a single shared count was
  5-40x slower than scanning with 16 threads. A reactive query notes where
  each segment ends, and on its first run, or if any segment received more
  entries since it last read it than it holds, scans as above; otherwise
  it walks the entries, running its body for each entity whose stamp still
  holds the entry's tick (its latest), that matches the query, and for
  which no earlier trigger of the query fired (so it runs once). With
  `parallel-entities`, it walks the segments in parallel once at least
  `parallel-min-events` entries are pending (default 16,384; an entity's
  latest entry is in one segment only, so no two iterations run its body).
  Writers
  stop appending to a segment once it is full for every reader, marking it
  as overflowed instead, which bounds what a frame that changes everything
  costs. Walking a log visits entities in the order events happened, so
  queries that combine applies or change which entities archetypes hold
  (both rely on row order) always scan; `explain=1` says so.
- A query that binds an optional component runs for every entity of the
  archetype and masks its stores with the presence, branch-free: every
  slot exists and belongs to the entity, so computing on an absent
  entity's stale values and keeping the old ones is safe. A body with an op
  that could be undefined on such values (division by a column, say) runs
  behind an `if` instead. `bench/churn/` measures why: the `if` form is 2-8x
  slower at medium densities.
- `parallel-entities=1` emits entity-local query loops as `scf.parallel`;
  `--convert-scf-to-openmp` turns them into OpenMP work-sharing loops, each
  alone in a parallel region. Such a loop ends with a barrier, and so does
  the region, where clang emits one for `parallel for`;
  `--ent-omp-nowait`, run after the conversion, drops the loop's (and
  `parallel-stages` emits its sections without one). With the OpenMP
  runtime's defaults the second barrier cost as much as the fork itself:
  it doubled the fixed cost of a parallel frame. A
  parallel loop only pays for its fork beyond some size, so an archetype
  whose capacity is below `parallel-min-entities` (default 1e6) always
  gets a sequential loop, and above it the count decides at run time. The
  default was the crossover on an M4 Max with the extra barrier; without
  it, parallel loops pay off from about 2e5 entities in the fused example
  and 4e5 unfused (`bench/RESULTS.md`), and the default is not lowered
  yet.

## Benchmarks

`bench/run.py` builds every variant of the example (per-system loops,
parallel stages, parallel entity loops, fused, fused with parallel entity
loops, and hand-written C with and without `restrict`), runs each
measurement in its own process, round-robin over sizes and variants, and
checks that all variants produce the same checksum.

```sh
python3 bench/run.py                    # OpenMP runtime defaults
python3 bench/run.py --blocktime 200    # keep OpenMP workers spinning
```

`bench/snn/run.py` runs the spiking network (`examples/snn.ent` pushing,
`examples/snn_pull.ent` gathering, sequential and parallel) against
hand-written C over compressed rows (push, pull, pull with OpenMP), per
network size and firing rate.

Homebrew's `libomp` defaults to `KMP_BLOCKTIME=0` with a passive wait
policy, so workers sleep after every parallel region; an empty region costs
about 35 us with 12 threads on an M4 Max, and about 1.7 us with
`KMP_BLOCKTIME=200`. Check that the machine is otherwise idle before
trusting a run. Results, with the conditions they were taken under, are in
`bench/RESULTS.md`.

## Milestones

- [x] **M0**: the dialect parses, prints and verifies.
- [x] **M1**: lower the toy simulation to loops and run it (from a C host
      rather than `mlir-runner`, which cannot call a schedule from an MLIR
      `main` before it is lowered).
- [x] **M2**: access analysis and a scheduling pass (compile-time parallelism).
- [ ] **M3**: layout choice (SoA/AoSoA), system fusion, benchmarks.
- [ ] Later: structural changes (spawn/despawn), change detection, relations,
      surface syntax.
