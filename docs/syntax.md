# ent-lang syntax (v1)

ent-lang source lives in `.ent` files. `ent-translate --import-ent` parses
one into the `ent` dialect, with locations pointing into the source, so
errors from later passes still name the `.ent` line. Everything here maps
directly onto the dialect described in the README; the semantics of when
effects become visible are in [`sync-points.md`](sync-points.md).

```sh
build/bin/ent-translate --import-ent examples/bullets.ent -o bullets.mlir
build/bin/ent-translate --ent-to-c-header bullets.mlir -o bullets_world.h
build/bin/ent-opt bullets.mlir --ent-lower-to-loops ...   # as in the README
```

`examples/*.ent` are the examples of `examples/*.mlir` written in ent-lang;
the integration tests run both and expect the same output.
`examples/filters.ent` (filters, `has`, `run_if`) and `examples/snn.ent` /
`examples/snn_pull.ent` (relations) exist only in ent-lang.

## Modules

```
import clock
import physics

system move() {
  for p: mut Position, v: Velocity {
    p.x += v.dx * clock::Time.dt
  }
}
```

A file is a module, named after the file: `import clock` loads
`clock.ent` from the directory of the importing file, or else from a
directory given to `ent-translate` with `-I dir`. Everything a module
declares is visible to the files that import it, under the name it has
there (`Position`, `tick()`); there is no access control yet.

- A file's own declaration comes before an imported one of the same name.
- A name that two imported modules declare is written with its module,
  `clock::Time`; any name may be.
- Only a file's own imports are in scope in it, not the imports of the
  modules it imports. A module is loaded once however many files import
  it; imports cannot form a cycle.
- The program is still one closed world: the compiler sees every module
  whole. In the IR and in errors a module's declaration is `clock.Time`,
  in the generated C header `ent_clock_Time_now`, `ent_clock_advance`
  (a schedule) and `ent_clock_sleep` (an extern system to define).
- A module may have a `world`. `main` and `default_capacity` belong to
  the program's own file.

A module with an `m.c` next to its `m.ent` is a device: the C defines
its extern systems, and `tools/ent` compiles it with the program. The
compiler's own are in `devices/` (`console`, `clock`), found without `-I`
by `tools/ent`.

## Declarations

```
component Position { x: f32, y: f32 } capacity 1000
tag Enemy                                   // a component without fields
unique Clock { dt: f32, frame: i64 }        // exists once (a resource)
unique Score: i64                           // shorthand: one field, `value`
archetype Gun { Position, optional Stunned } capacity 4
relation Synapse { weight: f32 } capacity 100000  // edges with data
relation Follows capacity 1000                     // edges without
relation Aims { w: f32 } from Ship to Target capacity 64  // typed ends
default_capacity 1024
```

- Types: `f32`, `f64`, `bool`, `i8`, `i16`, `i32`, `i64`, `index`, `entity`,
  `text[N]` (see Text below).
- `capacity` on a component bounds how many entities can have it; an
  archetype the compiler infers from spawns takes the smallest capacity
  among its required components, or `default_capacity`.
- Archetypes need only be declared for shapes the host or an extern system
  spawns (which the compiler cannot see), or to name and size one
  explicitly.
- A relation's edges go from a source entity to a target entity and carry
  its fields. They are not entities; `capacity` bounds how many there are.
  An entity may have any number of edges, also several to the same target.
  `from C` and `to D` name the components the sources and targets have;
  connecting checks them. Where no system despawns entities with `C` or
  removes it (and likewise for `D`), reading `C` of an edge's other end
  (`if let x = C(other).f`) is compiled without checking the id, though it
  is still written with `if let`.

## Systems and queries

```
system move(dt: f32) {
  for p: mut Position, v: Velocity {
    p.x += v.dx * dt
  }
}
```

A system takes parameters and runs statements; its access is inferred. It
may declare a contract the compiler checks: `system move(dt: f32) reads
Velocity writes Position { ... }`.

`for` (only at the top level of a system) visits every entity with the
bound components:

```
for e, p: Position, h: mut Hull with Enemy where h.hp < 10 on changed Hull.hp {
  ...
}
for e, h: mut Hull with Enemy, any(Fire, Ice) without Shield { ... }
for e with Enemy { ... }          // binds no component
for with Enemy { Count += 1 }     // nor the entity
```

- `e,` (optional, first) names the visited entity: `e` is its id, and
  `e.destroy()`, `e.add(Shield)`, `e.add(Stunned { seconds: 2.0 })`,
  `e.remove(Shield)` change it (deferred as the sync-point rules say).
  `e.has(Shield)` is a bool: whether the entity had `Shield` when the `for`
  started, so `if e.has(S) { e.remove(S) } else { e.add(S) }` toggles it
  however `S` is stored.
- `name: Component` binds a component read-only, `name: mut Component`
  writably.
- `with A, B` only filters: the entities must have them. `any(A, B)` in
  the `with` list: they must have at least one of them.
- `without A, B`: the entities must have none of them.
- Filters read only whether an entity has a component, never its fields, so
  a system filtering by `C` does not wait for one writing `C`'s fields. Where
  an archetype always or never holds the component, the compiler decides for
  the whole archetype; where it holds it optionally, per entity.
- `where cond` runs the body only where `cond` holds.
- `on changed C.f, changed C, added C, removed C` makes the query reactive;
  `log N` after a trigger sets its event log's capacity (`log 0`: none).

Inside a `for` that names its entity, another `for` visits the entity's
edges:

```
for e with Spiked {
  for s, post in e.out(Synapse) {         // edges from e; post: the target
    Neuron(post).input += s.weight
  }
}
for e, n: mut Neuron {
  for mut s, pre in e.in(Synapse) {       // edges to e; pre: the source
    s.weight *= 0.99                      // `mut`: the edge's fields
    if s.weight < 0.001 { s.disconnect() }
  }
}
```

Edges are visited in a fixed order: by source, then in the order they were
connected. `s.disconnect()` removes the edge when the outer `for` ends.
Edge loops do not nest, and a `for` may not visit a relation both ways if
either loop writes the edges. A `+=` into another entity's field inside an
edge loop runs once per edge.

## Statements

- `let name = expr`: an immutable local.
- `binding.field = expr`, and `+=`, `-=`, `*=`, `/=`, `min=`, `max=`.
- Uniques: `Clock.frame += 1`, `Score += 10` (the shorthand's value).
  Outside a `for` this reads and writes; inside one only `+=`, `-=`,
  `min=` and `max=` are allowed, and they accumulate (combined when the
  query ends, in a fixed order).
- Another entity: `Hull(target).hp -= damage` (also `+=`, `min=`, `max=`)
  combines into its field when the query ends; reading one may find nothing,
  so it is `if let hp = Hull(target).hp { ... } else { ... }`.
- `connect(a, b, Synapse { weight: 0.5 })` adds an edge from `a` to `b`
  (`connect(a, b, Follows)` without fields). Outside a `for` at once,
  inside one when the `for` ends (at most once per entity; not inside an
  edge loop).
- `spawn { Position { x: 1.0, y: 0.0 }, Velocity { dx: 2.0, dy: 0.0 } }`
  creates an entity; as an expression it returns its id
  (`let id = spawn { ... }`).
- `if cond { ... } else if cond { ... } else { ... }`.
- `for i in a..b { ... }` runs its statements with `i` = `a`, `a + 1`, ...
  `b - 1` (not at all if `a >= b`). The bounds are integers of one type,
  which `i` has too; literal bounds take the other bound's type, two
  literals count in `i32`. Outside a `for` over entities only; loops nest.

## Expressions

Literals (`1`, `2.5`, `1e8`, `true`), locals and parameters, `binding.field`,
`Unique.field`, `Unique` (shorthand), `- !`, `* / %`, `+ -`, comparisons,
`&&`, `||` (in that order of precedence, all left-associative),
`min(a, b)`, `max(a, b)`, `expr as T`, and
`if cond { let ...; value } else { value }`. A literal takes the type of the
operand it meets (`x * 2` is an f32 if `x` is); on its own an integer is an
i32 and a float an f32. There are no implicit conversions.

## Text

```
component Label { name: text[30] }
unique Title: text[62]

system describe() {
  for l: Label, h: Hull where l.name != "" {
    Title = "{l.name}: {h.hp} hp"
  }
}
```

`text[N]` holds up to N bytes (1 to 4094) and how many it holds. It is a
plain value like a number: stored in the column itself, copied whole, with
no lifetime, so a field of it costs its capacity for every entity (N plus
two bytes for the length, rounded up to 16). It is the type for bytes as
well as for text: any byte may be in it, nothing ends it.

- `"..."` is a literal, with the escapes `\n \t \r \0 \\ \" \' \{ \}` and
  `\xHH`. Put into a field or a unique it takes that capacity (and must
  fit); on its own (`let s = "abc"`) it is as long as it is.
- `"{value}"`: an expression in braces is put in as text. A text as it
  is; a bool as `true` or `false`; an integer in decimal; a float with up
  to six decimals, rounded (`0.25`, `2.0`), from 9e12 on without them,
  from 9e18 on as `big` (`inf` for infinity, `nan`).
- `a + b` joins two texts into one that holds both; `t += u` joins and
  stores.
- A text goes into a field, unique or parameter of another capacity:
  widened, or cut to the bytes that fit. `t as text[N]` does the same in
  an expression.
- `a == b`, `a != b` compare texts of any capacities, byte for byte.
- `len(t)` is the number of bytes (an `i32`); `t[i]` is byte `i` (an `i8`;
  0 past the end). `'a'` is a byte literal, with the same escapes.
- In the C header a text column is an array of `ent_textN`, a struct of
  `uint16_t length` and `char bytes[N]`, not terminated; C that writes one
  keeps the bytes past `length` zero. A text cannot be a parameter of a
  schedule a C host calls or of an extern system yet.

Text has no order (`<`), search or slices yet, and its bytes cannot be
changed one by one.

## Schedules

```
schedule frame(dt: f32) {
  shoot(dt)
  fly(dt)
}
```

Runs systems in order; arguments may be literals, which take the system
parameter's type. Systems must be declared before the schedules that run
them.

```
schedule frame(dt: f32) run_if !Paused {
  setup() run_if Clock.frame == 0
  move(dt)
}
```

`run_if cond` makes a run, or the whole schedule, conditional. The condition
is a bool over uniques and the schedule's parameters, evaluated when the run
(or the schedule) would start; its reads count as the run's when the
scheduler forms stages, and a conditional run is never fused with others.

## Extern systems

```
extern system report(scale: f32) reads Position, Clock writes Console
```

A system without a body, implemented in C as `void ent_report(ent_world
*world, float scale)` against the generated header, which declares it.
Schedules run it like any system. Since the compiler cannot see what it
does, what it declares is its access: every field of the components,
uniques and relations it names; an archetype in `writes` means it spawns
entities there. Without `reads` or `writes` it is ordered against every
other system. Output and other effects outside the world are not part of
the contract: extern systems that must stay in order because of them write
a common unique (`writes Console`), or declare nothing.

## The world

```
world {
  Clock.dt = 0.25
  for i in 0..100 {
    spawn { Position { x: i as f32, y: 0.0 }, Velocity { dx: 1.0, dy: 0.0 } }
  }
}
```

The state the program starts with: statements as in a system, run once.
With `main` they run before its first step, wherever the two are declared;
the worlds of imported modules run first, a module's after those of the
modules it imports. A C host runs them with `ent_world_init(world)` after
creating the world (and `ent_clock_world_init(world)` for a module
`clock`). (It is a system `world_setup` run by a schedule `world_init`,
which are therefore taken names.) A file has at most one `world`.

## The entry point

```
main {
  setup()
  loop {
    frame(Clock.dt)
  } until Clock.frame == 8 || Quit
  report()
}
```

A program with `main` runs on its own: the world is created, the steps run
in order, and the program returns 0. A step is a schedule call, whose
arguments are expressions over uniques, or a `loop`: it runs its steps,
then stops if the `until` condition (a bool over uniques) holds, so its
body runs at least once and the condition sees what it did. Without
`until` it repeats forever. `main` comes after the schedules it runs; the
`world`, if there is one, is set up first.
Without `main` the program is run by a C host through the generated
header, as before.

## Not yet supported

`fn`/`proc`, devices, prefabs, optional bindings (`T?`), mutable locals
(`var`), `while` loops and counted loops inside a `for` over entities: each
is reported as "not supported yet" where it would start. Of relations, not yet: traversal (`up`, `cascade`), joins
over relation variables, accumulating into a unique and connecting inside
an edge loop, and disconnecting by pair.
