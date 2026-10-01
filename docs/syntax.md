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

## Declarations

```
component Position { x: f32, y: f32 } capacity 1000
tag Enemy                                   // a component without fields
unique Clock { dt: f32, frame: i64 }        // exists once (a resource)
unique Score: i64                           // shorthand: one field, `value`
archetype Gun { Position, optional Stunned } capacity 4
relation Synapse { weight: f32 } capacity 100000  // edges with data
relation Follows capacity 1000                     // edges without
default_capacity 1024
```

- Types: `f32`, `f64`, `bool`, `i8`, `i16`, `i32`, `i64`, `index`, `entity`.
- `capacity` on a component bounds how many entities can have it; an
  archetype the compiler infers from spawns takes the smallest capacity
  among its required components, or `default_capacity`.
- Archetypes need only be declared for shapes the host spawns (which the
  compiler cannot see), or to name and size one explicitly.
- A relation's edges go from a source entity to a target entity and carry
  its fields. They are not entities; `capacity` bounds how many there are.
  An entity may have any number of edges, also several to the same target.

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

## Expressions

Literals (`1`, `2.5`, `1e8`, `true`), locals and parameters, `binding.field`,
`Unique.field`, `Unique` (shorthand), `- !`, `* / %`, `+ -`, comparisons,
`&&`, `||` (in that order of precedence, all left-associative),
`min(a, b)`, `max(a, b)`, `expr as T`, and
`if cond { let ...; value } else { value }`. A literal takes the type of the
operand it meets (`x * 2` is an f32 if `x` is); on its own an integer is an
i32 and a float an f32. There are no implicit conversions.

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

## Not yet supported

`fn`/`proc`, `world`, devices, prefabs, optional bindings (`T?`), mutable
locals (`var`), loops: each is reported as "not supported yet" where it
would start. Of relations, not yet: traversal (`up`, `cascade`), joins
over relation variables, accumulating into a unique and connecting inside
an edge loop, and disconnecting by pair.
