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

A module with an implementation next to its `m.ent` is a device: that
defines its extern procs and fns (and extern systems), and `tools/ent`
builds and links it with the program. The compiler's own are in `devices/`
(`console`, `clock`, `window`, `sound`, and `math`, see Math below), found
without `-I` by `tools/ent`; each says in its `.ent` file how it is used. The
implementation is any of:

- `m.c`: C, compiled against the generated headers.
- `m.cpp`: C++, the same way; the headers are `extern "C"`.
- `m.o`, `m.a`: an object or archive built elsewhere, linked as it is.
- `m.build`: one shell command that builds such an object or archive. It
  runs in the module's directory, with `ENT_OUTPUT` (the file to write),
  `ENT_INCLUDE` (the directory of the generated headers) and `ENT_CC`,
  `ENT_CXX` (the compilers `tools/ent` uses) set.

Next to any of them, `m.link` names the libraries the module needs, as
arguments for the linker (`-lraylib -lm`).

`tools/ent` compiles the program itself with the LLVM its IR was made
with, and hands the modules' C and C++ and the link to the machine's own
compiler (`cc`, `c++`, or what `ENT_CC` and `ENT_CXX` name), which knows
where the machine's libraries are.

So a device may be written in any language that can define a C function:
the contract is the C calling convention, with what the headers declare.
Such a function must not let an exception or a panic leave it, must not
need a runtime the program never started, and, for an `extern fn`, must
be callable from threads its language did not create.

## Declarations

```
component Position { x: f32, y: f32 } capacity 1000
tag Enemy                                   // a component without fields
unique Clock { dt: f32, frame: i64 }        // exists once (a resource)
unique Score: i64                           // shorthand: one field, `value`
enum Way { Right, Down, Left, Up }          // a type of named cases
archetype Gun { Position, optional Stunned } capacity 4
relation Synapse { weight: f32 } capacity 100000  // edges with data
relation Follows capacity 1000                     // edges without
relation Aims { w: f32 } from Ship to Target capacity 64  // typed ends
relation Orbits from Orbit to Body tree capacity 64       // one parent each
relation Flows from Node to Node tree sorted capacity 64  // stored in its order
default_capacity 1024
```

- Types: `f32`, `f64`, `bool`, `i8`, `i16`, `i32`, `i64`, `index`, `entity`,
  `text[N]` (see Text below), and the enums the program declares (see
  Enums below).
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
- A relation declared a `tree` gives every entity at most one edge out, to
  its parent, and lets none be its own ancestor. Connecting an entity that
  has a parent gives it the new one instead (the last `connect` wins); an
  edge that closes a cycle stops the program. A `for` can read up a tree
  and follow it in order (`up`, `cascade`, below).

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
- `name: Component up Relation` binds a component of another entity: of
  the nearest ancestor along the tree `Relation` that has it, which is the
  entity's parent, or else the parent's parent, and so on. The `for` visits
  only the entities with such an ancestor; the entity itself need not have
  the component, and may be filtered not to (`t: Tint up Orbits without
  Tint`). With `mut` (`down: mut Node up Flows`) its fields can be
  combined into (`+=`, `-=`, `min=`, `max=`), in a `for` that cascades
  along the tree.
- `cascade Relation`, after the filters, visits parents before their
  children along the tree: first the entities without a parent, then their
  children, and so on, each seeing what the `for` wrote before it.
  `cascade Relation leaves first` visits children before their parents.
  See Trees below.
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

### Trees

```
relation Orbits from Orbit to Body tree capacity 64

system place() {
  for b: mut Body, o: Orbit, center: Body up Orbits cascade Orbits {
    b.x = center.x + cos(o.angle) * o.distance
    b.y = center.y + sin(o.angle) * o.distance
  }
}

system draw() {
  for b: Body, t: Tint { circle(b.x, b.y, b.radius, t.color) }
  for b: Body, t: Tint up Orbits without Tint {
    circle(b.x, b.y, b.radius, t.color)
  }
}
```

`examples/orrery.ent` is this program: moons around planets around a sun.

A `for` sees other entities as they were when it started, which is why it
may not read a field of another entity that it writes itself: some would
have been visited before and some not. `place` does just that, and
`cascade` is what makes it mean something: the `for` runs as if it were
one `for` per depth of the tree, each seeing what those before it wrote.
So `center.x` is where the body's parent is this frame, however deep the
tree, and in whichever order the bodies were spawned. Without `cascade
Orbits` the compiler rejects `place`. `draw` reads `Tint`, which it does
not write, and needs no order.

- With a binding `up` the tree it cascades along, a `for` visits no entity
  without a parent, since that has no ancestor. Without one it visits
  those first.
- An entity whose parent was destroyed keeps its edge until the relation
  is next sorted (when an edge is connected or disconnected): until then
  it has no ancestor, and a `for` that only cascades visits it where the
  edge puts it.
- Where the relation says what its targets have (`to Body`) and nothing
  destroys bodies or takes `Body` away, `Body up Orbits` is the parent's,
  read without a search or a check.

The other way, from the leaves:

```
relation Flows from Node to Node tree capacity 100000

system run() {
  for n: mut Node { n.flow = n.rain }
  for n: Node, down: mut Node up Flows cascade Flows leaves first {
    down.flow += n.flow
  }
}
```

`examples/river.ent` is this: every node's flow becomes the rain on all
that is upstream of it. `leaves first` runs the depths from the deepest to
the entities without a parent, so a node has what all above it sent when
it passes its own on. What goes into an ancestor through a `mut` binding
lands when the depth that sent it is through, combined in the order the
`for` visits the entities, which is fixed; so the `for` may read the field
of its own entity, but not through a binding `up` the tree or with `if
let`, where it would see what others of its depth sent. An ancestor's
field is only combined into, never assigned.

**Sorted trees.** A tree may ask for its entities to be stored in its
order:

```
relation Flows from Node to Node tree sorted capacity 100000
```

The archetypes that hold them, declared or not, then have their rows in
the tree's order (the entities without a parent first, then the others by
their depth), and a `for` that cascades along the tree reads them one
after another and finds each parent where it is, instead of walking a
list of ids: the river's sum takes 0.4x the time at a million nodes in a bushy
tree (`bench/RESULTS.md`). Nothing else about the program changes; it is a
choice of storage, and it has its price:

- Every change puts the rows in order again, all of them with all their
  fields: a `connect` or disconnect of the tree, and a `spawn` or
  `destroy` in the archetype (outside a `for`, before the next `for` or
  when the system ends; in a `for`, when it ends). At a million nodes in
  a bushy tree that is some 24 ms, 18 of them for the edges, which are
  sorted with or without it: dozens of passes over the tree. In deep,
  narrow trees the order gains nothing.
- Rows are not the order of spawning any more, and move. What is combined
  in the order of rows (`+=` into another entity or a unique in a plain
  `for`) is combined in an order that depends on the tree, which shows in
  the last bits of floats; a C host cannot keep a row across a call.
- No entity's id is its row in such a program: reading another entity
  (`if let`, `+=`) goes through a table, everywhere.
- Which archetypes are sorted follows from what the relation says its
  ends have (`from`, `to`, which it must, and which nothing may remove):
  every archetype with either. A tree in one archetype is one pass over
  its rows. A tree in several (the orrery's bodies come in three shapes)
  is read depth by depth, in each the rows every archetype has of it: in a
  bushy tree that costs a fifth more than in one archetype and is still
  less than half of unsorted; in a deep, narrow one it is twice as slow
  as unsorted, a loop for every few rows. Across archetypes, what
  children send to a parent is combined by archetype and then by row, not
  in the order unsorted has, so sums of floats differ in their last bits
  between the two.
- An archetype's rows have one order: two sorted trees with their
  entities in the same archetype are an error. Any number of other
  relations, and of trees that are not sorted, are no trouble, since edges
  hold ids and not rows.
- Destroying an entity sorts, so the edges of its children are dropped at
  once and they are without a parent right away, where otherwise they
  keep a dangling edge until the tree next changes.

Not yet: besides that, a cascading `for` only reads and writes fields. It
does not `spawn`, `destroy`, `add` or `remove`, combine into an entity
found by its id or into a unique, `connect` or disconnect, and it has no
`on`; it visits one entity after another, on one core. Combining into an
ancestor needs the `for` to cascade along that tree.

## Statements

- `let name = expr`: an immutable local.
- `var name = expr`, `var name: type = expr`: a local that can be assigned,
  with `=`, `+=`, `-=`, `*=`, `/=`, `min=`, `max=` (a text with `=` and
  `+=`). It has the type of the value it starts with, or the one written:
  `var best: i64 = 0`, `var line: text[30] = ""`. See Vars below.
- `binding.field = expr`, and `+=`, `-=`, `*=`, `/=`, `min=`, `max=`.
- Uniques: `Clock.frame += 1`, `Score += 10` (the shorthand's value).
  Outside a `for` this reads and writes; inside one only `+=`, `-=`,
  `min=` and `max=` are allowed, and they accumulate (combined when the
  query ends, in a fixed order).
- Another entity: `Hull(target).hp -= damage` (also `+=`, `min=`, `max=`)
  combines into its field when the query ends; reading one may find nothing,
  so it is `if let hp = Hull(target).hp { ... } else { ... }`.
- `connect(a, b, Synapse { weight: 0.5 })` adds an edge from `a` to `b`
  (`connect(a, b, Follows)` without fields). Outside a `for` it is there
  for the next `for` and everything after; inside one, when the `for` ends
  (at most once per entity; not inside an edge loop). A system that
  connects in a loop outside a `for` sorts the edges once, before its next
  `for` or when it ends, which is also when a tree is found to have a
  cycle, or more edges than its capacity is counted with the edges that a
  later `connect` of the same entity replaces.
- `spawn { Position { x: 1.0, y: 0.0 }, Velocity { dx: 2.0, dy: 0.0 } }`
  creates an entity; as an expression it returns its id
  (`let id = spawn { ... }`).
- `if cond { ... } else if cond { ... } else { ... }`.
- `for i in a..b { ... }` runs its statements with `i` = `a`, `a + 1`, ...
  `b - 1` (not at all if `a >= b`). The bounds are integers of one type,
  which `i` has too; literal bounds take the other bound's type, two
  literals count in `i32`. Outside a `for` over entities only; loops nest.

### Vars

```
system steer() {
  for g: mut Ghost {
    var best = 1000000
    var dir = g.dir
    if !wall(Maze, g.col + 1, g.row) {
      let d = distance(g.col + 1, g.row, Player.col, Player.row)
      if d < best { best = d  dir = 0 }
    }
    // ... and the other three ways
    g.dir = dir
  }
}

world {
  var pellets = 0
  for i in 0..868 {
    if Maze[i] == '.' { pellets += 1 }
  }
  Left = pellets
}
```

A `var` lives to the end of the block it is declared in, like a `let`.
It may be assigned in that block and in the `if`s (also `if let`) and
counted `for`s inside it; after an `if` it holds what the branch that ran
left in it, after a loop what the last round did.

- A `var` declared in the body of a `for` over entities is that entity's
  own: every entity starts with a fresh one.
- A `var` from outside a `for` over entities can be read in it but not
  assigned: every entity would assign it, in an order that is not the
  program's to choose. What entities add up goes into a unique (`+=`,
  `-=`, `min=`, `max=`).
- A `let` keeps the value a var had where the `let` was made.
- A var is not storage: the compiler follows its values, and reading and
  assigning one cost what the values cost.

Not yet: assigning, inside an edge loop, a var from outside it; and vars
in a branch of an `if` that gives a value, which is made of `let`s.

## Expressions

Literals (`1`, `2.5`, `1e8`, `true`), locals and parameters, `binding.field`,
`Unique.field`, `Unique` (shorthand), `- !`, `* / %`, `+ -`, comparisons,
`&&`, `||` (in that order of precedence, all left-associative),
`min(a, b)`, `max(a, b)`, `expr as T`, and
`if cond { let ...; value } else { value }` (also with `else if cond
{ value }` between). A literal takes the type of the
operand it meets (`x * 2` is an f32 if `x` is); on its own an integer is an
i32 and a float an f32. There are no implicit conversions.

An integer may be written in hex (`0xff8800`): the bits of the type it
takes, so `0xff` is an i8 (all its bits, the number -1) and a ninth bit
does not fit one. It is never a float.

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
  schedule a C host calls or of an extern system yet; an extern fn or proc
  takes one.

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
*world, float scale)` against the generated header, which declares it. It
gets the whole world, and the compiler cannot check that it keeps to what
it declares: where values are enough, an extern fn or proc is the safer
way out.
Schedules run it like any system. Since the compiler cannot see what it
does, what it declares is its access: every field of the components,
uniques and relations it names; an archetype in `writes` means it spawns
entities there. Without `reads` or `writes` it is ordered against every
other system. Output and other effects outside the world are not part of
the contract: extern systems that must stay in order because of them write
a common unique (`writes Console`), or declare nothing.

## Fns

```
fn square(x: f32) -> f32 { x * x }

fn distance(ax: f32, ay: f32, bx: f32, by: f32) -> f32 {
  let dx = ax - bx
  let dy = ay - by
  square(dx) + square(dy)
}

fn wall(maze: text[868], col: i32, row: i32) -> bool {
  maze[row * 28 + col] == '#'
}

system chase() {
  for g: mut Ghost where !wall(Maze, g.col + 1, g.row) {
    g.far = distance(g.x, g.y, Player.x, Player.y)
  }
}
```

A `fn` computes a value from its parameters. Its body is statements and
then the value it gives, which has the type after `->`:

```
fn pellets(maze: text[868]) -> i32 {
  var n = 0
  for i in 0..len(maze) {
    if maze[i] == '.' { n += 1 }
  }
  n
}

fn clamp(x: i32, low: i32, high: i32) -> i32 {
  var y = x
  if y < low { y = low } else if y > high { y = high }
  y
}
```

- The statements are those that compute: `let`, `var` and assignments to
  vars, `if` with `else if` and `else`, and counted `for`s, nested as in a
  system.
- The last thing in the body is the value. An `if` that starts it is a
  value (and so has an `else`): after its last branch comes the body's
  `}`, or what continues a value (`if a { 1 } else { 2 } + x`, `... as
  f32`). Any other `if` is a statement. A `-` after an `if` starts the
  next thing; to subtract from an `if`, write it in parentheses.

- It never sees the world: no uniques, no components, no `for` over
  entities, no `spawn`, no procs. What it needs of them a system passes in, so what a system reads
  and writes is still what its own body says. A text is passed like any
  value, whole.
- A call is computing like arithmetic: the same result for the same
  arguments and nothing else. It may run on several threads at once, be
  moved, or be dropped if its value is not used, and it does not keep a
  `for` from running on all cores.
- Parameters and the result are of any type; a literal argument takes the
  parameter's type, a text its capacity (widened or cut).
- A fn may give several values: `-> (i32, i32)`, with `(col, row)` as what
  it gives, see Several values below.
- A fn is declared before it is called. It may call other fns, also
  `extern` ones, and itself: `fn gcd(a: i64, b: i64) -> i64 { if b == 0
  { a } else { gcd(b, a % b) } }`. Nothing bounds how deep.
- Fns are called in systems, `world` and other fns, not in `main` or a
  `run_if` condition.
- A module's fns are visible to the files that import it, like its other
  declarations. C does not hear of them: they are in no header.

`proc` with a body in ent-lang is not supported yet.

### Several values

```
fn step(col: i32, row: i32, way: Way) -> (i32, i32) {
  (col + dx(way), row + dy(way))
}

fn target(kind: Kind, scatter: bool, pcol: i32, prow: i32) -> (i32, i32) {
  if scatter { (26, 0) }
  else if kind == Kind.Pink { step(pcol, prow, Way.Up) }
  else { (pcol, prow) }
}

system chase() {
  for g: mut Ghost {
    let (tcol, trow) = target(g.kind, Phase.scatter, Pac.col, Pac.row)
    var (col, row) = step(g.col, g.row, g.dir)
    ...
  }
}
```

A fn gives several values by naming their types in parentheses after
`->`. What it gives is then `(a, b)`, a call of a fn that gives the same,
or an `if` whose branches each give them. `let (a, b) = ...` takes them
apart into locals, `var (a, b) = ...` into vars; the right side is again a
call, `(a, b)` or such an `if`.

There are no tuples besides: several values are not a value. They cannot
be a field, a parameter or a local, and a call that gives several cannot
stand where one value is expected. An `extern fn` gives one value, as C
does.

## Enums

```
enum State { Ready, Playing, Over }
enum Way { Right, Down, Left, Up }

component Ghost { way: Way, col: i32 }
unique Game { state: State }

fn back(way: Way) -> Way { ((way as i32 + 2) % 4) as Way }

system turn() {
  for g: mut Ghost where g.way != Way.Up {
    g.way = back(g.way)
  }
}

schedule frame() {
  turn() run_if Game.state == State.Playing
}
```

An enum is a type whose values are its cases, written `Way.Left`. It is a
type of its own: a `Way` goes where a `Way` is expected and nowhere else,
neither a number nor another enum's case.

- It is the type of fields, uniques, parameters, results, locals and
  vars. A field starts as the first case, as a number starts as 0.
- Values are compared with `==` and `!=`, in systems, fns and in the
  conditions of `run_if` and `main`. There is no order and no arithmetic.
- `way as i32` (or another integer type) is the number of the case, the
  first being 0, and `n as Way` the case with that number; a number no
  case has gives a value equal to none of them.
- In a text, `"{g.way}"` is the name of the case.
- An enum has 1 to 256 cases and is stored as one byte. An `extern fn` or
  `proc` takes and gives one; in the generated C header `Way` is
  `ent_Way` (a `uint8_t`) with `ent_Way_Right`, `ent_Way_Down`, ... for
  the cases, and a module `m`'s is `ent_m_Way`.
- An enum is declared before it is used. A module's enums are visible to
  the files that import it, like its other declarations.

## Math

```
import math

system orbit() {
  for p: mut Position, o: Orbit {
    p.x = o.cx + cos(o.angle) * o.radius
    p.y = o.cy + sin(o.angle) * o.radius
  }
}
```

The module `math` (devices/math.ent) has the usual functions, all fns:
they compute, and may be called wherever a fn may. A name as it is works
on `f32`, with `64` at its end on `f64` (`sin64`), with an `i` in front on
`i32`. Angles are radians.

- From the machine's math library (devices/math.c), each also with `64`:
  `sqrt`, `pow(x, y)`, `exp`, `log`, `log2`, `sin`, `cos`, `tan`, `asin`,
  `acos`, `atan`, `atan2(y, x)`, `floor`, `ceil`, `round`, `trunc`,
  `fmod(x, y)`.
- Written in ent-lang: `pi()`, `tau()` (and `pi64()`, `tau64()`),
  `radians(degrees)`, `degrees(radians)`, `abs`, `sign`, `clamp(x, low,
  high)`, `lerp(a, b, t)` (each also with `64`, and `iabs`, `isign`,
  `iclamp`), `fract`, `length(x, y)`, `distance(ax, ay, bx, by)`,
  `ipow(x, n)`.
- `min` and `max` are built in and need no import.
- A program's own fn of the same name comes before the module's; the
  module's is then `math::abs`.

## Extern fns and procs

```
extern fn noise(x: f32, y: f32) -> f32
extern proc put(line: text[126])
extern proc wait_until(due: f64) -> f64

system write() {
  for p: Print { put(p.line) }
}
system wobble() {
  for p: mut Position { p.y += noise(p.x, p.y) }
}
```

Functions implemented in C, which systems call. They take values and give
at most one back; they never get the world. So what a system reads and
writes is what its own body does, inferred and checked as always, and the C
has nothing to get wrong about it. Prefer them to extern systems.

- An `extern fn` computes like a `fn`: the same result for the same
  arguments, and nothing else. The compiler takes that on trust and treats
  a call like arithmetic: it may run on several threads at once, be moved,
  or be dropped if its value is not used. It always gives a value.
- An `extern proc` acts on the outside (output, input, the time) and may
  give a value back. It is called exactly where and as often as the
  systems say: a system that calls one keeps its place in the schedule
  among all other systems, and a `for` that calls one visits its entities
  in order, one at a time. A proc is called as a statement, or as an
  expression where it gives a value.
- They are called in systems and in `world`, not in `main` or a `run_if`
  condition: those read uniques, which a system has written.
- Parameters and results are numbers and bools; a parameter may also be a
  text, which C gets as `const ent_textN *`. A text argument of another
  capacity is widened or cut like anywhere else.
- In C, `extern proc put` is `void ent_put(const ent_text126 *line)`, and
  a module `console`'s is `ent_console_put`. `tools/ent` generates their
  declarations as `ent_extern.h` (`ent-translate
  --ent-to-c-extern-header`): the texts and the functions, without the
  world. They are also in the world's header.

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

`proc` with a body, `device` declarations, prefabs, optional bindings (`T?`),
`while` loops and counted loops inside a `for` over entities: each
is reported as "not supported yet" where it would start. Of relations, not
yet: joins over relation variables, accumulating into a unique and
connecting inside an edge loop, disconnecting by pair, and of trees what
Trees above lists.
