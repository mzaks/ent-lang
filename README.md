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

On Linux any LLVM 22 that ships MLIR and its CMake files will do. Where the
distribution packages none (Arch packages LLVM without MLIR), conda-forge
has it; with [pixi](https://pixi.sh), into the ignored `build/`:

```sh
mkdir -p build/toolchain && cd build/toolchain
pixi init . -c conda-forge
pixi add mlir=22.1.8 llvmdev=22.1.8 clang=22.1.8 clangxx=22.1.8 \
         llvm-openmp=22.1.8 lld=22.1.8 lit ninja
cd ../..
LLVM=$PWD/build/toolchain/.pixi/envs/default
# conda-forge's LLVM leaves out the test tools; the distribution's llvm
# package (same major version) has them.
for t in FileCheck not count split-file; do ln -s /usr/bin/$t $LLVM/bin/; done
cmake -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=$LLVM/bin/ninja \
      -DMLIR_DIR=$LLVM/lib/cmake/mlir -DLLVM_DIR=$LLVM/lib/cmake/llvm \
      -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      -DENT_LIT=$LLVM/bin/lit -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cmake --build build --target check-ent
```

`ent-opt` and `ent-translate` are compiled with the system's clang against
the environment's libraries; the tests and benchmarks compile with the
environment's clang. Tested on Arch Linux, x86-64. Removing `build/`
removes the toolchain with it. Below, set `LLVM` to that prefix instead of
Homebrew's.

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
output; `examples/filters.ent`, for filters, `has` and `run_if`,
`examples/snn*.ent`, for relations, and `examples/orrery.ent`,
`examples/river.ent` and `examples/layout.ent`, for trees, exist only in
ent-lang):

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

## A program of its own

```
import console
import clock

system greet() {
  spawn { Print { line: "hello from frame {Time.frame}" } }
}

schedule frame() { tick()  greet()  write()  clear() }

main {
  loop { frame() } until Time.frame == 5
}
```

```sh
tools/ent run examples/hello.ent       # or: tools/ent build ... -o hello
```

`tools/ent` runs the steps below for a program and the modules it imports
(`--parallel` stages and fuses the systems and runs them on all cores,
`--keep dir` keeps the IR and the header). It finds the compiler in
`build/bin` and LLVM under `$LLVM_PREFIX`, `build/toolchain` or Homebrew.

A program with a `main` needs no C host: the compiler emits C's `main`,
which creates the world and runs the schedules as `main` says, after the
statements of `world`, the state the program starts with. What the
language cannot do itself (input, output, the time) is an `extern proc`,
and a computation better written in C an `extern fn`: functions
implemented in C that systems call with values. They never get the world,
so everything that reads or writes it is ent-lang, and checked. A `fn`
with a body is the same kind of function written in ent-lang:

```
extern proc put(line: text[126])
extern proc wait_until(due: f64) -> f64
extern fn noise(x: f32, y: f32) -> f32
fn square(x: f32) -> f32 { x * x }

system write() {
  for p: Print { put(p.line) }
}
```

An `extern system` is the older, wider door: a system declared in
ent-lang, with its access, and implemented in C against the generated
header, working on the world itself. The compiler takes its declared
access on trust. `examples/bullets_main.ent` is the bullets example that
way; its one extern system prints the result
(`examples/extern/bullets_report.c`).

```
world {
  for i in 0..2 {
    spawn {
      Position { x: i as f32 * 100.0 },
      Cooldown { seconds: i as f32 * 0.5, period: 1.0 }
    }
  }
}

extern system report() reads Position, Cooldown, Lifetime

schedule finish() { report() }

main {
  loop { frame(0.25) } until Clock.frame == 8
  finish()
}
```

```sh
build/bin/ent-translate --import-ent examples/bullets_main.ent -o /tmp/bm.mlir
build/bin/ent-translate --ent-to-c-header /tmp/bm.mlir \
    -o /tmp/bullets_main_world.h
build/bin/ent-opt /tmp/bm.mlir --ent-lower-to-loops --convert-scf-to-cf \
    --convert-to-llvm --reconcile-unrealized-casts \
  | $LLVM/bin/mlir-translate --mlir-to-llvmir -o /tmp/bm.ll
$LLVM/bin/clang -O2 -Wno-override-module -I/tmp /tmp/bm.ll \
    examples/extern/bullets_report.c -o /tmp/bm && /tmp/bm
```

A program may be several files: `import clock` loads `clock.ent`, found
next to the importing file or in an `-I` directory, and puts what it
declares in scope (`clock::Time` where two modules declare a `Time`). The
whole program is still compiled as one closed world; see
[`docs/syntax.md`](docs/syntax.md).

A device is a module that touches the outside: its systems in
`name.ent`, and in `name.c` next to it the extern procs and fns they
call, which `tools/ent` compiles against the declarations generated for
them (`ent_extern.h`, which has no world in it). Instead of `name.c` it
may be `name.cpp`, an object or archive (`name.o`, `name.a`), or a command
that builds one (`name.build`), so a device can be written in any language
that defines C functions; see [`docs/syntax.md`](docs/syntax.md). Three
come with the compiler, in `devices/`, and two more that need
[raylib](https://www.raylib.com) installed:

- `math`: `sqrt`, `sin`, `cos`, `atan2`, `floor`, `pow` and the like from
  the machine's math library, and `abs`, `clamp`, `lerp`, `length`, `pi()`
  and more written in ent-lang on top. All are fns, for `f32` by their
  plain name and for `f64` with `64` at its end.

- `console`: a program prints a line by spawning `Print { line: "..." }`;
  `write()` puts the pending lines out in order, `clear()` takes them
  away.
- `clock`: `tick()`, once a frame, sets `Time.now` (seconds since the
  first tick), `Time.dt` and `Time.frame`, and waits for the next frame if
  `FrameRate` is set.

- `window`: a window to draw in, with the keys and the mouse.
  `begin()` opens it, fills `Window`, `Mouse` and `Keys` and clears the
  picture; systems then draw with `rect`, `circle`, `sector`, `line` and
  `label` (colours are `0xRRGGBB`); `present()` shows the frame and holds
  `Window.fps`. `examples/bounce.ent` is a program with it:
  `tools/ent run examples/bounce.ent`; `examples/orrery.ent` has moons
  around planets around a sun, each placed from where the body it circles
  is; `examples/layout_demo.ent` is a window of boxes inside boxes, with
  labels the window measures and a note that floats at the mouse, drawn
  depth first (`top down dfs Inside`) and laid
  out by the library `examples/layout.ent` (fixed, fit and grow sizes in
  rows and columns, after Clay) in three reactive `for`s along a tree
  whose children are in an order (`tree ordered by Slot.at`), each
  running only where something it reads changed, in the box, in the one
  it is in (`(b: Box)-[Inside]->(outer: Box)`), in the one before it
  (`(prev: Box)~[Inside]~>(b)`) or in one inside it:
  `tools/ent run -I examples examples/layout_demo.ent`; and `examples/pacman.ent` is a whole game: the maze is a text that fns read, the pellets and ghosts are
  entities, ways, modes and the game's state are enums, and the only C is
  that of the devices and the math module. A module's `name.link` names the
  libraries it needs (`-lraylib`).

- `sound`: a program makes a sound by spawning a `Tone { pitch, to,
  seconds, after, volume, wave }`, made up on the spot (a square,
  triangle or sine wave or a hiss, sliding from one pitch to another), or
  a `Sample { file, volume }`, a sound file; `play()`, once a frame,
  starts what is pending. `Sound.volume` and `Sound.off` are for
  everything. `examples/pacman.ent` has its tune and noises from it,
  without a file.

All keep their state in the world, and their logic in ent-lang: their C is
`put` and `flush` for the console, `wait_until` for the clock, for the
window one small function for each call into raylib, and for sound `tone`
and `sample`.

## In an editor

`build/bin/ent-lsp` is a language server for `.ent` files: it runs the
compiler's front end as a file changes and gives an editor its errors and
warnings, the outline, where a name is declared, and on hovering over a
name its declaration and comment, and for a system what it reads and
writes and what it waits for in a schedule. `editors/` has what VS Code
and Zed need to use it and to colour the source (a TextMate grammar and a
tree-sitter grammar); see [`editors/README.md`](editors/README.md).

## The dialect today

When each effect becomes visible to the rest of the program (the frame
model: queries as the unit of consistency, their end as the commit point)
is specified in [`docs/sync-points.md`](docs/sync-points.md).

- `ent.component @Position (x: f32, y: f32) capacity 100000`: scalar fields,
  abstract layout; the optional capacity bounds how many entities can have
  the component. A field may be a text, `name: !ent.text<30>`: up to 30
  bytes and their number, stored inline as one integer (the length in its
  low 16 bits, the bytes above, zero past the length), which is what the
  lowering turns the type into; `text[30]` in ent-lang, whose literals,
  joins and comparisons are integer ops on it (`docs/syntax.md`).
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
  %b (%w)` adds an edge (outside queries for every query that follows, inside at the query's
  end), `ent.disconnect` inside an edge loop removes the visited one at the
  query's end. A relation may name the components its ends have
  (`ent.relation @Synapse (weight: f32) from @Neuron to @Neuron`):
  connecting checks them, and where no system despawns entities with such a
  component or removes it, lookups and applies through an edge's other end
  of that component skip the checks that the id is alive and has it.
  `examples/snn.ent` and `examples/snn_pull.ent` are a spiking
  neural network pushing spikes along outgoing synapses and gathering them
  along incoming ones; both agree with a plain C simulation to the bit.
- `ent.relation @Orbits () from @Orbit to @Body tree capacity 64` is a
  tree: an entity has at most one edge out, to its parent, and none is its
  own ancestor. Connecting an entity that has a parent gives it the new one
  (the last connect wins); an edge that closes a cycle stops the program.
  A query argument `%c: !ent.ref<@Body, up @Orbits>` is a read-only ref to
  `@Body` of the nearest ancestor that has it (the parent, else the
  parent's parent, ...), and the query visits only the entities with such
  an ancestor. `ent.query (...) cascade @Orbits { ... }` visits parents
  before their children: it runs as if it were one query per depth of the
  tree, each seeing what those before wrote, so it may read through a ref
  up the tree a field it writes itself, which is otherwise rejected like a
  lookup of one. `cascade @Orbits leaves first` visits children before
  their parents. `ent.combine %down "flow" add %v : !ent.ref<@Node, mut, up
  @Flows>, f32` combines a value into a field of the ancestor a `mut` ref
  leads to, in a query cascading along that tree; the values land when the
  depth that sent them is through. So do `ent.apply` and `ent.accumulate`
  in a cascading query, which may then not read what it sends to that way;
  `ent.despawn`, an `ent.add` or `ent.remove` that moves the entity,
  `ent.connect` and `ent.disconnect` take effect when the whole query has
  run, as in any query (the rows of entities that go or move are marked as
  they are visited and listed in order at the end), and `ent.spawn` at
  once, its entity not visited. A cascading query may be reactive, and
  then also react to the events of the ancestor a ref leads to
  (`on [changed @Local, changed @World "x" up @Under]`): an ancestor's
  stamp is read where the ancestor is, so what the query wrote there a
  depth before counts, and a change goes down the tree in one pass. An
  entity given another parent counts as well (the relation keeps the tick
  of each entity's last connect, and an event log of them). Such
  a query goes from the entities in its triggers' event logs, and from
  each one it changes, down to the children: it marks them in a bitmap
  over the tree's order (an unsorted tree's list, with the children's
  links; the rows of a tree sorted in one archetype, with each row's
  range of child rows) and visits the marked ones in that order, so its
  time follows the entities it comes to; a tree sorted in several
  archetypes has a bitmap per archetype and for every row its children's
  rows in each, and is gone through depth by depth. Otherwise (`leaves
  first`, no log, a log that lost events, a deep tree in several
  archetypes)
  it goes through the whole tree and runs its body where a trigger
  fired.
  It runs on one core unless its tree is sorted in one archetype (see
  Fusion and entity parallelism).
  A tree may be `sorted` (`... from @Node to @Node tree sorted capacity
  N`): the archetype that holds its entities, declared or inferred, keeps
  its rows in the tree's order, every entity after its parent, and a query
  cascading along the tree is a loop over those rows, with each entity's
  parent at a row it reads from a column, and no ids. It is a choice of
  storage with a price: the rows are put in order again whenever the
  tree's edges change or the archetype gains or loses an entity, ids are
  never rows in such a program, and no archetype is in two sorted trees.
  The tree names what its ends have; where several archetypes hold them,
  each has its rows by depth and the query goes depth by depth. `docs/syntax.md` lists what follows.
  `examples/orrery.ent` places moons from their planets and planets from
  their sun that way, and draws a body without a colour in that of the
  nearest body up the tree that has one; `examples/river.ent` sums the
  rain on everything upstream of each node, from the leaves.
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
- `ent.extern @report(f32) reads [@Position] writes [@Console]`: a system
  without a body, implemented in C. A schedule runs it like any other, and
  the lowered program calls `ent_report(world, dt)` (a bool crosses as C's
  `bool`, an entity as `ent_entity`), which works on the world through the
  generated header. Its declared access is its access: every field of the
  components, resources and relations it lists, in every archetype holding
  them, and for an archetype in `writes` its set of entities (it spawns
  them, so the archetype is declared, like any shape only C spawns).
  Without `reads` or `writes` it conflicts with every other system. What
  it does outside the world (output, say) is not in the contract: extern
  systems that must keep their order because of it write a common
  resource, or declare nothing. Field writes it makes are not tracked for
  reactive queries, as for a host; edges it connects are sorted when it
  returns.
- `ent.function proc @put(!ent.text<126>)`, `ent.function @noise(f32, f32)
  -> f32`: a function implemented in C, and `ent.invoke proc @put(%line) :
  (!ent.text<126>) -> ()` its call from a system. The lowered program
  calls `ent_put(&line)`: values only (a bool as C's `bool`, a text as a
  pointer to a copy of it), at most one back, never the world. A plain
  function gives the same result for the same arguments and does nothing
  else, so its calls are computing like any arithmetic: they may run on
  several threads, or be dropped. A `proc` acts on the outside: a system
  that calls one keeps its place among all other systems, is not fused,
  and a query that calls one visits its entities in order.
  `ent.function @square(%x: f32) -> f32 { ... ent.yield %y : f32 }` is a
  plain function with a body, the program's own: its parameters are the
  body's arguments and `ent.yield` gives its value. The body only
  computes, calling other plain functions or itself, and is lowered to a
  private function `ent_square`, which gets its values as they are (a
  text as the integer that holds it) and which LLVM inlines where that
  pays. It may give several values (`-> (f32, i32)`), which are then the
  results of the function and of its calls. Its calls are `ent.invoke` like any other, so access analysis,
  staging and parallel loops treat both kinds alike.
- `ent.enum @Way ["Right", "Down", "Left", "Up"]` declares an enum, and
  `!ent.enum<@Way>` is the type of its values: a type of its own for
  fields, parameters and results, stored as the byte that numbers the
  case. As with a text and its integer, a value is computed with as that
  byte through `builtin.unrealized_conversion_cast`, which lowering
  removes. A generated C header has `ent_Way` and its cases.
- `ent.main { ent.call @setup()  ent.loop { ent.call @frame(%dt) : f32 ...
  ent.yield %done : i1 } }`: the entry point, at most one. It calls
  schedules, repeats (`ent.loop` runs its body, then stops if it yielded
  true; without a value it repeats forever), reads resources and computes
  with ops free of side effects. Lowering emits C's `main`: it allocates
  the arena, zeroes its header as `ent_world_create` does, runs the body
  and returns 0. A module without `ent.main` is a library for a C host, as
  before.

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
conflict with everything. An extern system's access is what it declares
(`--ent-print-access` shows the columns); without a declaration it is
opaque.

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
region with one `omp.section` per run. A run of an extern system becomes a
call of its C function, with the arena's pointer first; `ent.main` becomes
`main` (so no system or schedule may be called that).

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
them, which LLVM cannot tell from the arena's views. With both, and the
relation's ends typed, the pull example takes 1.01-1.05x the time of
hand-written C (6.3-9.4x before; see `bench/RESULTS.md`).

A tree has no such table unless it is `sorted`. Every entity key has a
slot for its entity's one edge (its source's id and one, so that a new
world's zeroes are no edges; the target's id; the fields), the edges to an
entity are a list through their sources' slots, in the order they were
connected, and the ids of the entities with a parent are in a list,
parents before children, each with its parent's id next to it and its
place in a column. A connect keeps all of that as it goes: the slot is
set, the entity joins its new parent's children, and the list takes a new
leaf at its end or a new parent's id in place where the parent is before
the entity. Where the parent comes later, or the entity has children and
was not in the list, the entity goes to the list's end with everything
below it, found from child to child with the end of the list for a queue;
the entries left behind are all ones, which a walk skips, and the list
has room for twice the edges. Where that room runs out, and for a
disconnect or a host's connect, the relation is marked unclean and built
again from its slots before anything reads it: edges of and to dead entities dropped, children by key, and the
list by key, each entity after those of its ancestors that were not in
yet (most are: a look at the parent's slot, and no walk through the
tree). An entity on a cycle would wait for itself, which stops the
program. A `sorted` tree keeps
the table, sorted by source with the index by target, and is sorted again
for every change.

A despawned entity's edge and the edges to it are taken out of such a tree
with the entity (its slot cleared, its children's slots cleared and their
entries in the list set to all ones), so no edge of the tree has a dead
end. That makes the ends of a tree's edges trusted whatever the program
despawns, as long as it does not remove the component the relation names:
a cascading query over a tree whose nodes can be destroyed finds them and
their parents without checking a generation. Its ids are slots, so where
an entity is would come from the entity table; instead the tree's list has
each entity's packed location and its parent's next to their ids, noted
when the entry is made. When rows of an archetype that can hold the tree's
entities move (a despawn's swap, a move, a sort), the tree is marked
stale, and where the relation is next looked over the locations are read
from the entity table again, once. Such a tree takes 1.2-1.3x the time of
one whose nodes cannot go at 1e6 nodes when bushy, where it took 5.7x
(8.9x in two archetypes; see `bench/RESULTS.md`). A cascading
query first runs a loop per archetype for the entities without a parent,
then walks that list (`leaves first`: the list from its end, then that
loop), finding each entity by its id, without a check where the
relation's sources cannot die and can live in one archetype only. A ref
up the tree is read like a lookup of the ancestor, found by following
parents until one has the component, or, where the relation's targets are
trusted to have it, of the parent directly, which the list has at hand.
On the river (`bench/river/`) that takes 0.96-1.02x the time of C written
the same way at 1e6 nodes and 1.15-1.26x in the caches.

C that stores the nodes themselves in the tree's order takes 0.40-0.46x
the time at 1e6, and so does the program with its tree `sorted`. The
relation's sort then also moves the archetype's rows: the entities without
a parent to the front, as they are, the others behind them as listed,
through a copy of every column; the entity table follows, and a column
takes each row's parent's row. A cascading query over it is one loop over
the rows behind the roots, up or down, which reads the parent's fields at
that row: 1.01-1.04x the time of the C. A sort costs 8-24 ms at 1e6 nodes,
most of it for the edges, which are sorted either way; in a bushy tree the
order pays where the tree changes less often than every dozen steps, in a
deep one never.

A sorted tree in several archetypes cannot have every parent before its
children in one archetype's rows, since the parent may be in another. Each
archetype then has its rows by depth, an array saying where each depth
starts, and per row its parent's packed location; a cascading query is a
loop over the depths holding a loop per archetype, and reads the parent
through a branch on its archetype. With the river's nodes in two
archetypes that takes 1.2x the time of the one-archetype sorted tree when
the tree is bushy, which is 0.42x of the same tree unsorted at 1e6; a deep
tree across archetypes is better left unsorted, sorted taking 1.9x the
time (see `bench/RESULTS.md`).

An entity that is known to be alive and to have a component (the trusted
end of an edge, an id from a tree's list where nothing dies) is found
without a check: in its one archetype directly, or by a branch on its
archetype where it can be in several. Checking the row against a loaded
count behind such branches made the unsorted river in two archetypes
4-8x slower than in one; without, it takes 1.0-1.2x.

A loop that never runs in parallel visits entities in the order the
query's end combines applies in. So an apply whose field nothing else in
the query touches (no get, set or lookup of it, no second apply to it, no
add or remove of its component), and an accumulate whose resource field
nothing else in the query reads or accumulates into, are combined as the
loop visits, without buffers and the second pass (same result). Loops
that may run in parallel keep the buffers; `direct-applies=0` keeps them
everywhere. Within an entity loop that never runs in parallel, a resource
cell such an accumulate combines into is carried as a loop value and
stored once after the loop; and where both branches of an `if` store to
the same column at the same index, the value is chosen in the `if` and
stored once after it. Both keep loops vectorisable that LLVM otherwise
would not vectorise, since it cannot tell the arena's views apart. With
these, the spiking network matches hand-written C over compressed rows,
push and pull, from 1e4 to 1e6 neurons (see `bench/RESULTS.md`).
Connects append and mark the relation unclean; a sort (a stable counting
sort through scratch columns, O(edges + keys)) runs where edges changed:
at a schedule's start for edges the host connected, before a system's
next query and at its end where it connected outside queries (once,
however many connects: sorting after each made building a tree of n
entities cost n sorts), and at the end of a query that connected or
disconnected. A sorted tree's sort does not count and sort twice: it
notes each source's last edge and goes through the keys in order. And a
connect of an entity that had an edge when the tree was last sorted sets
that edge in place, at the entity's offset, instead of appending one for
the sort to prefer: giving an entity another parent then needs no room in
a table that is full. It
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
per resource field such as `ent_Clock_frame(world)`; one entry point per
schedule, such as `ent_frame(world, dt)`; and a declaration of every extern
system's C function, such as `void ent_report(ent_world *world, float
arg0)`, and of every extern fn and proc, for the file that defines it.
`ent-translate --ent-to-c-extern-header` emits the declarations of the
extern fns and procs alone, with the texts they take: what the C of a
device includes. The header allocates ids exactly as
the lowered program does. Creating a world zeroes only the counts, resources
and entity counters, so capacity costs address space, not memory, until
columns are written; and, for every tree that is not sorted, the owners of
its slots (an id's width per entity key), which say whether a slot holds
an edge and must not be whatever the memory held. Parallel stages and loops assume nothing else writes the
arena while a schedule runs.

## Entity ids

The compiler picks how ids are represented from the capacities and from
which structural changes the program makes:

- **Rows**: nothing is despawned or moved and no tree is `sorted`, so an
  entity keeps its row and its id is `archetype << rowBits |
  row`. No id column, no entity table.
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

- A cascading query over a sorted tree in one archetype can run a depth
  in parallel with `parallel-entities=1`: the sort notes where each depth
  starts in the rows, and for every row the rows of its children, which
  are next to each other. Where the body is local to a depth (gets and
  sets, refs up the tree, `ent.combine`, reads; nothing deferred, no
  proc), the archetype's capacity is at least `parallel-min-entities` and
  a depth holds 256 entities on average, the query goes depth by depth
  instead of row by row, and a depth of at least `parallel-min-level`
  entities (default 32,768) is an `scf.parallel`: over its rows, or, where
  the body combines into the parent, over 64 pieces of them cut at rows
  where a parent's children begin, each piece its rows in order, so every
  parent's sum is one thread's and has the order it always has. A deep
  tree, with few entities a depth, keeps its one loop. On the river at
  1e6 nodes that takes 0.47x the time of the one loop with 12 threads and
  0.37x with 4 on the fast cores (see `bench/RESULTS.md`).

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

The scripts take the toolchain from `LLVM_PREFIX` (default: Homebrew's),
so on Linux: `LLVM_PREFIX=$PWD/build/toolchain/.pixi/envs/default python3
bench/run.py`.
Every binary is built for the machine's own instruction set
(`-march=native`, on Arm `-mcpu=native`); `BENCH_CFLAGS` gives other flags
instead (`BENCH_CFLAGS=` for a baseline build, on x86 SSE2; results before
2026-10-06 are baseline unless they say native). On a CPU with two kinds
of cores,
pin the run (`taskset -c 2 python3 bench/run.py`, or `OMP_PLACES=cores
OMP_PROC_BIND=close` for the parallel variants): unpinned, spreads reached
60% on a Ryzen AI 9 HX 370.

`bench/river/run.py` runs the river network (`examples/river.ent`, a sum
down a tree from its leaves) against hand-written C, per size and shape of
the tree; `bench/river/edges.py` runs edge loops over the same tree, kept
as a tree and as a table of edges. `bench/scene/run.py` runs a scene
graph placed by a cascading `for` every step against the same `for` made
reactive.

`bench/snn/run.py` runs the spiking network (`examples/snn.ent` pushing,
`examples/snn_pull.ent` gathering, sequential and parallel) against
hand-written C over compressed rows (push, pull, pull with OpenMP), per
network size and firing rate.

Homebrew's `libomp` defaults to `KMP_BLOCKTIME=0` with a passive wait
policy, so workers sleep after every parallel region; an empty region costs
about 35 us with 12 threads on an M4 Max, and about 1.7 us with
`KMP_BLOCKTIME=200`. Check that the machine is otherwise idle before
trusting a run. With conda-forge's `libomp` on
Linux `--blocktime 200` made no measurable difference, and forks were as
cheap by default as on the Mac with it. Results, with the conditions they were taken under, are in
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
