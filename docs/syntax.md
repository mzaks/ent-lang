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
- A module's `for` that matches no entity the program can have is left
  out without a word (a library has `for`s for what a program may not
  use); for one of the program's own the compiler says so.

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
relation (Ship)-[Aims { w: f32 }]->(Target) capacity 64   // typed ends
relation (Orbit)-[Orbits]->(Body) tree capacity 64        // one parent each
relation (Node)-[Flows]->(Node) tree sorted capacity 64   // stored in its order
relation (Box)-[Inside]->(Box) tree ordered by Slot.at capacity 64  // children in order
default_capacity 1024
capacity Inside 4096                        // another capacity for one declared before
```

- Types: `f32`, `f64`, `bool`, `i8`, `i16`, `i32`, `i64`, `index`, `entity`,
  `text[N]` and `text` (see Text below), and the enums the program declares (see
  Enums below).
- A whole number goes where one of more bits is expected as it is (an
  `i32` where an `i64` is, as a value, an argument or one side of `+`
  or `<`: two of different sizes are computed with as the bigger). The
  other way round takes `as`.
- `none`, where an entity is expected, is no entity: the same as itself
  (`target == none`) and as no other; what is sent to it goes nowhere,
  and what is looked up of it is not found.
- `capacity` on a component bounds how many entities can have it; an
  archetype the compiler infers from spawns takes the smallest capacity
  among its required components, or `default_capacity`.
- `capacity Name N` gives a component, a relation or an archetype that
  was declared before, in the file or in a module it imports, another
  capacity: a module cannot know how much the program that imports it
  needs. The last one said holds, so a program's over a module's, and a
  module's over that of one it imports.
- Archetypes need only be declared for shapes the host or an extern system
  spawns (which the compiler cannot see), or to name and size one
  explicitly.
- A relation's edges go from a source entity to a target entity and carry
  its fields. They are not entities; `capacity` bounds how many there are.
  An entity may have any number of edges, also several to the same target.
  `(C)-[Name]->(D)` names the components the sources and targets have
  (either may be left open, `()`); connecting checks them. Where no system despawns entities with `C` or
  removes it (and likewise for `D`), reading `C` of an edge's other end
  (`if let x = C(other).f`) is compiled without checking the id, though it
  is still written with `if let`.
- A relation declared a `tree` gives every entity at most one edge out, to
  its parent, and lets none be its own ancestor. Connecting an entity that
  has a parent gives it the new one instead (the last `connect` wins); an
  edge that closes a cycle stops the program. A `for` can read up a tree
  and follow it in order (arrows in its head, `top down` and `bottom up`,
  below).

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

`for` (at the top level of a system, or in a counted `for`, a `loop` or
an `if` there) visits
every entity with the bound components:

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
- `optional name: Component` (after the first binding, or in the first
  node: `(e, b: Box, optional t: Label)`) binds a component the entities
  may be without: the `for` visits those without it too, and its fields
  are read with `if let v = name.field { ... } else { ... }`. Only read;
  to change it, visit the entities that have it in a `for` of their own.
- `with A, B` only filters: the entities must have them. `any(A, B)` in
  the `with` list: they must have at least one of them.
- `without A, B`: the entities must have none of them.
- Filters read only whether an entity has a component, never its fields, so
  a system filtering by `C` does not wait for one writing `C`'s fields. Where
  an archetype always or never holds the component, the compiler decides for
  the whole archetype; where it holds it optionally, per entity.
- With arrows, the head is patterns, and binds components of other
  entities too. A node is an entity in parentheses with what is bound of
  it; the first node is the entity the `for` visits.

  ```
  for (b: mut Box, s: Size)-[Inside]->(outer: Box, k: Stack) { ... }
  ```

  `-[Relation]->` leads to the entity's parent along a tree: `outer` and
  `k` are the parent's `Box` and `Stack`, and the `for` visits only the
  entities whose parent has both. The entity itself need not have what
  is bound of the parent, and may be filtered not to. With `mut`
  (`(n: Node)-[Flows]->(down: mut Node)`) a field can be combined into
  (`+=`, `-=`, `min=`, `max=`), in a `for` that goes along the tree (`top down`, `bottom up`).
- `-[Relation*]->` leads to the nearest ancestor that has what is bound:
  the parent, or else the parent's parent, and so on
  (`(b: Body)-[Orbits*]->(t: Tint)`).
- Further patterns follow after commas, each from or to the visited
  entity, which is named by its own name or one of its bindings:
  `(prev: Box)~[Inside]~>(b)` binds the sibling before, the child of the
  same parent that comes right before the entity in a tree whose children
  are in an order (`tree ordered by`), if it has the component. It is
  read, never written.
- Arrows go on up from the visited entity, each a step from the node
  before it: `(b: Box)-[Inside]->(outer: Box)-[Inside]->(far: Box)` binds
  the parent's and the parent's parent's, and a node on the way may be
  left empty, `()`, where nothing of it is read. A step may be `*`, to
  the nearest ancestor of the node before it that has all its node binds
  (`(t: Thing)-[Inside]->(floor: Thing)-[Inside*]->(c: Tint)`: the
  nearest above the floor with a `Tint`), and may be along another tree
  (`(room)-[Inside]->(home)-[Feeds]->(p: Power)`). What several arrows,
  or a `*` to a node that binds several components, lead to is read,
  not written.
- An arrow that goes on can also lead to a sibling of the node before
  it, along a tree whose children are in an order:
  `(b)-[Inside]->(outer)~[Inside]~>(aunt: Box)` binds what the box after
  the one `b` is in has, `(b)-[Inside]->()<~[Inside]~(uncle: Box)` the
  one before it. Read, not written; and since a sibling on the way may
  be one the `for` has yet to visit, it does not read from it a field it
  writes.
- The other way along the siblings, `(b)~[Inside]~>(next: Box)` binds the
  sibling after. It is yet to be visited: a `for` does not read from it a
  field it writes.
- A node at the other end can name its entity, alone or before what it
  binds: `(b: Box)-[Inside]->(parent)`, `(parent, outer: Box)`. The name
  is the entity's id, to send a value to (`Box(parent).w += 1.0`) or to
  connect; by itself it takes the relation to say what that end has.
- Two entities are compared with `==` and `!=`, whatever names them: the
  one a `for` visits, one at the other end of an arrow, one a field or a
  unique holds (`if into != ez.up { ... }`).
- `optional` before a pattern: the `for` also visits the entities without
  that parent, ancestor or sibling, and what the pattern binds is read
  with `if let v = name.field { ... } else { ... }`. See Trees below.
- After the first pattern, an arrow can be written either way round,
  `(outer: Box)<-[Inside]-(b)` and `(b)<~[Inside]~(prev: Box)`: it is the
  arrow's direction that says which end is which. An arrow to the visited
  entity (from its children) is not for the head: see the loop below.
- `top down Relation`, after the filters, visits parents before their
  children along the tree: first the entities without a parent, then
  those below them, each seeing what the `for` wrote before it. `bottom
  up Relation` visits children before their parents. Which of two
  entities that are not above one another comes first is then the tree's
  business. With `bfs` or `dfs` before the relation it is said:

  | clause | order |
  |---|---|
  | `top down bfs R` | depth by depth, the children of one entity next to each other |
  | `bottom up bfs R` | the same from its end |
  | `top down dfs R` | an entity, then all that is below it, then the next |
  | `bottom up dfs R` | all that is below an entity, then the entity |

  See Trees below.
- `where cond` runs the body only where `cond` holds.
- `on changed C.f, changed C, added C, removed C` makes the query reactive.
  A component can be named by its binding (`changed b.w`), and another
  entity's event is asked that way (`changed outer.x`, in a `for` that
  goes along a tree: see Trees below);
  `log N` after a trigger sets its event log's capacity (`log 0`: none).

Inside a `for`, another `for` over entities runs its body for every
entity of a kind, for the entity the outer one visits:

```
for me, t: Tower, a: mut Aim {
  var best = t.range * t.range
  var target = me
  var found = false
  for e, en: Enemy without Hidden
      where (en.x - t.x) * (en.x - t.x) + (en.y - t.y) * (en.y - t.y) < best {
    best = (en.x - t.x) * (en.x - t.x) + (en.y - t.y) * (en.y - t.y)
    target = e
    found = true
  }
  a.found = found
  if found { Life(target).hp -= t.damage }
}
```

- It has a name for the entity (optional, first), bindings, `with`,
  `without` and `where`; no `mut`, `optional`, `on`, arrows or order. It
  visits its entities in a fixed order, all of them for every entity of
  the outer `for`: as many runs as the two numbers multiplied.
- It reads its entities. What the outer body finds it keeps in its own
  vars, which the inner body may assign: they go round with the loop.
  The inner entity's name is a value (`target = e`), to send to
  (`Life(target).hp -= damage`) or to keep in a field. A var that is to
  say which entity starts as `none`.
- The outer entity's own fields are read and written in it as outside.
- What it sends to another entity (`Health(foe).hp -= harm`, to every
  foe near a blast) and what it accumulates into a unique (`Hits += 1`)
  lands as it goes, once for each of its entities; so nothing else in
  the outer `for` reads or sets that field, or reads that unique.
- Connecting, spawning, destroying, `add` and `remove` are for after the
  loop, not in it.
- It may bind a component the outer `for` binds `mut`, and read the
  fields that one does not set. A field the outer one sets it reads only
  as it was before the outer `for` started, with `old`:

  ```
  for me, s: mut Spot {
    var sum = 0.0
    var n = 0.0
    for o, other: old Spot where o != me {
      sum += other.x
      n += 1.0
    }
    if n > 0.0 { s.x += (sum / n - s.x) * 0.1 }
  }
  ```

  Without `old` that is an error, since of the others some would be set
  already and some not. With it a copy of the column is kept for as long
  as the outer `for` runs, and the inner one reads the copy: every entity
  sees the others as they were, at the price of the copy. (Or the new
  value goes into another field, and a second `for` copies it over.)
- One deep: no `for` over entities inside it, and none inside a `for`
  over edges. A `for` with one in it that only reads runs in parallel
  like any other (from fewer entities: its work is the two numbers
  multiplied); one whose inner `for` sends or accumulates runs on one
  thread, in order.

Inside a `for`, another `for` with an arrow visits the entity's edges.
One end is the visited entity, by its name or one of its bindings; the
arrow says whether the edges go out of it or come into it:

```
for e with Spiked {
  for (e)-[s: Synapse]->(post) {          // edges from e; post: the target
    Neuron(post).input += s.weight
  }
}
for n: mut Neuron {
  for (pre)-[mut s: Synapse]->(n) {       // edges to n; pre: the source
    s.weight *= 0.99                      // `mut`: the edge's fields
    if s.weight < 0.001 { s.disconnect() }
  }
}
for c: mut Content {
  for (inner: Box)-[Inside]->(c) {        // the boxes inside this one
    c.width += inner.w
  }
}
```

In the brackets: the relation, before it a name for the edge where its
fields are read (`s: Synapse`), and `mut` where they are written. The
other end is `(name)` for its id, `()` for nothing, or binds components of
it (`(inner: Box)`, `(pre, n: Neuron)`): the body then runs for the edges
whose other end has them, and a field is read like any other
(`inner.w`). With `mut` (`(inner: mut Box)`) a value can be sent to a
field, `inner.w += 1.0`, which lands when the outer `for` ends, like
`Box(id).w += 1.0`.

Edges are visited in a fixed order: by source, then in the order they were
connected (a tree's edges into an entity: in the order they were
connected). `s.disconnect()` removes the edge when the outer `for` ends.
Edge loops do not nest, and a `for` may not visit a relation both ways if
either loop writes the edges. A `+=` into another entity's field inside an
edge loop runs once per edge.

### Trees

```
relation (Orbit)-[Orbits]->(Body) tree capacity 64

system place() {
  for (b: mut Body, o: Orbit)-[Orbits]->(center: Body) top down Orbits {
    b.x = center.x + cos(o.angle) * o.distance
    b.y = center.y + sin(o.angle) * o.distance
  }
}

system draw() {
  for b: Body, t: Tint { circle(b.x, b.y, b.radius, t.color) }
  for (b: Body)-[Orbits*]->(t: Tint) without Tint {
    circle(b.x, b.y, b.radius, t.color)
  }
}
```

`examples/orrery.ent` is this program: moons around planets around a sun.

A `for` sees other entities as they were when it started, which is why it
may not read a field of another entity that it writes itself: some would
have been visited before and some not. `place` does just that, and
`top down` is what makes it mean something: the `for` runs as if it were
one `for` per depth of the tree, each seeing what those before it wrote.
So `center.x` is where the body's parent is this frame, however deep the
tree, and in whichever order the bodies were spawned. Without `top down
Orbits` the compiler rejects `place`. `draw` reads `Tint`, which it does
not write, and needs no order.

- With an arrow up the tree it goes along, a `for` visits no entity
  without a parent, since that has no ancestor. Without one it visits
  those first.
- Which of two entities that are not above one another comes first is
  the order they were connected in: a tree takes most changes in where
  they happen, a new leaf at the end of its order, a node that gets
  another parent where it is. So two programs that build the same tree in
  another order may add a parent's inflows in another order, and their
  sums of floats differ in the last bits. A node put under one that
  comes after it, or given a parent when it has children, goes to the end
  with everything below it. (A disconnect has the order made again, by
  the entities' ids with every entity's ancestors before it; so do edges
  a host connected, and an order that has run out of room for nodes that
  moved.) A `sorted` tree's order is its shape's alone.
- Destroying an entity takes its edge and the edges to it out of the
  tree when the `for` that destroyed it ends: its children have no parent
  from then on, with all that is below them as it was. (Other relations
  keep the edges of a destroyed entity until they are next sorted.)
- Where the relation says what its targets have (`to Body`) and nothing
  destroys bodies or takes `Body` away, `-[Orbits*]->(c: Body)` is the
  parent's too,
  read without a search or a check.

The other way, from the leaves:

```
relation (Node)-[Flows]->(Node) tree capacity 100000

system run() {
  for n: mut Node { n.flow = n.rain }
  for (n: Node)-[Flows]->(down: mut Node) bottom up Flows {
    down.flow += n.flow
  }
}
```

`examples/river.ent` is this: every node's flow becomes the rain on all
that is upstream of it. `bottom up` runs the depths from the deepest to
the entities without a parent, so a node has what all above it sent when
it passes its own on. What goes into an ancestor through a `mut` binding
lands when the depth that sent it is through, combined in the order the
`for` visits the entities, which is fixed; so the `for` may read the field
of its own entity, but not through an arrow up the tree or with `if
let`, where it would see what others of its depth sent. An ancestor's
field is only combined into, never assigned.

**Children in an order.** A tree may give the children of an entity an
order, by an integer field of theirs:

```
component Slot { at: i32 }
relation (Box)-[Inside]->(Box) tree ordered by Slot.at capacity 256

for (b: mut Box)-[Inside]->(outer: Box), optional (prev: Box)~[Inside]~>(b)
    top down Inside {
  b.x = outer.x
  if let x = prev.x, let w = prev.w { b.x = x + w }
}
```

`examples/layout.ent` places boxes this way: each where the one before
it in the same box ends, the first where the box they are in starts.

- The children of an entity are ordered by the field, smallest first. A
  child without the component counts as 0, and children with the same
  value are in the order they were connected: a tree nobody gives slots
  is in connect order.
- A `for` that goes along the tree `top down` visits the children of an
  entity in that order (not `bottom up`), and so does an
  edge loop over the edges into an entity. So such a `for` may read
  from the sibling before what it writes, as it may from the parent: the
  sibling
  before has been visited.
- `~[R]~>` is from the sibling right before, and only that: if it does not
  have the component, the binding is not there (the `for` does not visit
  the entity, or with `optional` finds nothing), and no earlier sibling
  is taken instead.
- With every pattern along the tree `optional`, a cascading
  `for` visits the entities without a parent too, first, as one without
  such bindings does.
- Reordering is a write to the field, in any `for`. The tree puts its
  children in order again before the next `for` after a write to the
  field and after a `connect`: all of the tree, some milliseconds for a
  million nodes, so this is for trees that are reordered now and then.
  A host that writes the field through the header is not noticed until
  something is connected.
- Together with `sorted` (`tree sorted ordered by Slot.at`), the rows
  are in the tree's order with the children of an entity in theirs: the
  sibling before is the row before. Across several archetypes such a
  tree is read by its list.
- Not yet: an order by a float.

**An order asked for exactly.** `top down` and `bottom up` alone promise
only that parents come before their children, or after: which of two
nodes that are not above one another comes first is as the tree is
stored, and costs nothing. Where it shows (a `for` that draws, sends,
prints or adds up floats), `bfs` or `dfs` says it:

```
for b: Box, t: Tint top down dfs Inside {
  rect(b.x, b.y, b.w, b.h, t.color)      // a box, then all that is in it
}
```

- `bfs` is depth by depth, the entities without a parent first (last,
  `bottom up`); in a depth the children of one entity are next to each
  other, in the order the tree has them. `dfs` is an entity and then all
  that is below it (`bottom up`: all that is below it and then the
  entity), child by child in the tree's order. Both give the same order
  whether the tree is `sorted`, `ordered`, both or neither.
- The rules about reading what the `for` writes are those of the
  direction: `top down` may read the parent and the sibling before,
  `bottom up` the children.
- A tree that is `sorted` or `ordered` is stored breadth first, so `bfs`
  costs it nothing. Every other exact order is worked out from the tree
  and kept until the tree changes (a connect, a destroyed node, another
  order of siblings); the entities are then found one by one. At a
  million nodes a `for` in such an order takes 1.2 ms where the one
  without takes 0.6, and 10-17 ms in a step that changed the tree
  (`bench/RESULTS.md`). One order is kept per tree: two `for`s that ask
  for different ones work theirs out in turn. Such a `for` runs on one
  core and, if reactive, goes through the whole tree.

**Sorted trees.** A tree may ask for its entities to be stored in its
order:

```
relation (Node)-[Flows]->(Node) tree sorted capacity 100000
```

The archetypes that hold them, declared or not, then have their rows in
the tree's order (the entities without a parent first, then the others by
their depth), and a `for` that goes along the tree reads them one
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
  ends have (which it must, and which nothing may remove):
  every archetype with either. A tree in one archetype is one pass over
  its rows. A tree in several (the orrery's bodies come in three shapes)
  is read depth by depth, in each the rows every archetype has of it: in a
  bushy tree that costs a fifth more than in one archetype and is still
  less than half of unsorted. A deep, narrow one (fewer than 64 nodes a
  depth) is read by the tree's list instead, every node with where its
  row and its parent's are, which costs up to 1.7x of one archetype.
  Across archetypes, what
  children send to a parent is combined by archetype and then by row, not
  in the order unsorted has, so sums of floats differ in their last bits
  between the two.
- An archetype's rows have one order: two sorted trees with their
  entities in the same archetype are an error. Any number of other
  relations, and of trees that are not sorted, are no trouble, since edges
  hold ids and not rows.

A cascading `for` may also do what any `for` does to the world beyond its
own entity:

```
for (e, n: Node)-[Under]->(parent: Node) top down Under
    where parent.total > 100 {
  Pruned += 1                // into a unique
  Stats(Keeper).lost += n.local   // into an entity it has the id of
  e.destroy()
}
```

- `+=`, `-=`, `min=`, `max=` into a unique, and into a field of another
  entity (`Component(id).field += v`), land when the depth that sent them
  is through, in the order the `for` visits the entities. So the `for` may
  not read a unique it adds into, nor read or set a field it sends to
  that way, of any entity: an entity of the same depth would see what
  those before it sent.
  A `for` that goes depth first (`top down dfs R`, `bottom up dfs R`)
  has one order, an entity and all below it before the next, and may
  read a unique it adds into: each entity sees what those before it
  added (`let line = Next` and `Next += 1` number them as they come).
  That is the order where it reacts too, among those it runs for.
- `e.destroy()` takes effect when the whole `for` has run, as in any
  `for`: every entity is visited as the tree was when it started, a
  destroyed node's children too, with their parent still there to read.
- So does everything else that changes which entities there are, what
  they have, or the tree: `e.add(C)` and `e.remove(C)` where they move
  the entity to another archetype, `connect` (also of a node the `for`
  has just spawned, to the node it visits) and `s.disconnect()` in an
  edge loop. `spawn` makes its entity at once, and the `for` does not
  visit it. A tree can be grown, pruned and rearranged from a `for` that
  walks it, and is walked as it was.

A cascading `for` may be reactive, and then react to what happened to
the ancestor it binds as well:

```
system place() {
  for (w: mut World, l: Local)-[Under]->(above: World)
      top down Under
      on changed l, changed above.x {
    w.x = above.x + l.x
  }
}
```

`changed above.x` is the event of the entity `above` is taken
from: the body runs for a node whose own `Local` changed since the `for`
last ran, or whose ancestor's `World.x` did, which includes what this
`for` wrote when it visited the ancestor a moment before. So moving one
node places it and everything below it again in the same pass, and no
other node's body runs. What the `for` wrote into ancestors it has
thereby passed down: the next time it runs that is no event.

- Only `changed` can be asked of another entity; the `for` must go along
  the tree the binding is reached by.
- `changed next`, of a binding to the sibling after, runs the `for` for
  a node whose sibling after had the event, or which has another sibling
  after it than it had. That sibling is visited after the node whichever
  way the `for` goes, so its events count like the node's own: the next
  time the `for` runs. The `for` need not go along the tree.
- `changed prev`, of a binding from the sibling before, is the same for
  that sibling: the `for` runs for a node whose
  sibling before had the event, also in the same pass, or which has
  another sibling before it than it had (one came, went or was moved).
- `changed (: C.f)-[Relation]->(name)`, in a `for` that goes `bottom
  up`, is the event of any of the node's children (no binding stands
  for them in the head, so a pattern says it), also one the `for` has just
  caused in a child; a child that comes or goes is one too. No binding
  goes with it: a node reads its children in an edge loop,

  ```
  for n: mut Node bottom up Under
      on changed n.own, changed (: Node.total)-[Under]->(n) {
    n.total = n.own
    for (child: Node)-[Under]->(n) { n.total += child.total }
  }
  ```

  which it may though the `for` writes `total`: where a `for` goes
  along a tree, the other end of the tree's edges it visits has been
  visited (the children `bottom up`, the parent `top down`).
  In a `for` that goes `top down` the children are visited after the
  node: their events count for it the next time it runs, like its own.
- A binding along another tree than the one the `for` goes along (or of
  a `for` that goes along none) has its events too,
  `(e)-[Over]->(under: Box) ... on changed under.w`: they count the next
  time the `for` runs, since nothing says which of the two is visited
  first.
- An event that counts the next time is not lost, and no entity reads
  an old value for it: what a `for` reads of another entity where its
  order says nothing (a sibling after, a child `top down`, along another
  tree) it may not change. Where one thing is to follow from another in
  the same frame both ways, that is two `for`s, each reading what the
  other writes, in a `loop` until neither has anything to do
  (`loop { let a = for ... { }  let b = for ... { } } until a + b == 0`).
- `changed` means changed: a write of the value a field has is no
  event. So passes that feed each other (as in `examples/layout.ent`,
  where sizes go up the tree and room comes down) settle by themselves,
  each running only as long as something still gets another value.
  (Compared where the field is written, and only for fields some `for`
  reacts to. A text is not compared: writing one counts.)
- A binding several arrows up has its events too
  (`(b)-[R]->()-[R]->(far: C) ... on changed far.x`): the `for` runs for
  an entity when what it finds there changed, and when an entity on the
  way was put under another parent, since it then finds another; with
  a `*`, also when one on the way lost what the `*` asks for, or got it
  (also for the one arrow `-[R*]->`, unless the relation says that every
  parent has the component).
- What a node sees through the binding also changes when the node is
  given another parent: `connect (node)-[Under]->(other)` is such an event
  for `node`, whether a system or the host connects it, and the `for`
  places it and everything below it again the next time it runs.
- Such a `for` goes only where the events lead: to the nodes that had
  one, and from each node it changes down to its children; over an
  unsorted tree also on to the sibling after (a trigger on the sibling
  before's) and, `bottom up`, to the parent (a trigger on a child's),
  with the nodes that have no parent last. Its time
  follows the number of nodes it comes to, not the tree's size: at a
  million nodes, moving one takes 5 us where placing all of them takes
  630, and 4 against 235 where the tree is `sorted` (`bench/RESULTS.md`,
  the scene graph). Per node it comes to it costs far more than a pass over all of
  them does, though, since it jumps where that reads on: once more than
  a node in a hundred or so is placed again, the `for` without `on` is
  the faster one, up to four times where most of the tree moves.
  In `examples/layout.ent` every pass is such a `for`: with 90,000
  boxes of which one is another height each frame, a frame takes 18 us,
  against 1,500 going through all of them (`bench/boxes`).
- Over a `sorted` tree the events are followed by its rows where the
  `for` goes `top down` with one arrow to the parent and triggers on
  itself and on that; with a trigger on a child or a sibling, `bottom
  up`, with no arrow up, with arrows that go on or a `*` to a component
  not every parent has, or `bfs` or `dfs`, they are followed in a
  depth-first order that is worked out and kept until the tree changes
  (and everything is still gone through by the rows).
- It goes through the whole tree instead, running its body where a
  trigger fired: where a trigger's event log has capacity 0 or has lost
  events, on its first run, with events for more than a sixteenth of
  the tree, and where more than 256 nodes got or lost a child or a
  sibling since it last ran. (And with a way up that has two `*` along
  a `sorted` tree.) That saves the body, not the walk.
- It runs on one core.

Not yet: combining into an ancestor needs the `for` to go along
that tree.

A cascading `for` visits one entity after another, with one exception:
over a `sorted` tree in one archetype, built with parallel loops
(`tools/ent --parallel`), the entities of one depth are visited on all
cores where the body only reads and writes the entity and what is above
it (fields, what arrows bind, `+=` into an ancestor, reads of uniques and of
other entities), the archetype has room for a million entities and the
depth holds 32,768 or more. The result is the same to the bit: what
children send to a parent is still added in their order. Over a `sorted`
tree in several archetypes the same holds for each archetype's entities
of a depth.

## Statements

- `let name = expr`: an immutable local.
- `var name = expr`, `var name: type = expr`: a local that can be assigned,
  with `=`, `+=`, `-=`, `*=`, `/=`, `min=`, `max=` (a text with `=` and
  `+=`). It has the type of the value it starts with, or the one written:
  `var best: i64 = 0`, `var line: text[30] = ""`. See Vars below.
- `binding.field = expr`, and `+=`, `-=`, `*=`, `/=`, `min=`, `max=`.
- Uniques: `Clock.frame += 1`, `Score += 10` (the shorthand's value).
  Outside a `for` this reads and writes; inside one `+=`, `-=`, `min=`
  and `max=` accumulate (combined when the query ends, in a fixed
  order), and `=` sets it to what the last entity that sets it gives, in
  the order of the `for`, when the `for` ends: a number, a bool, an enum
  or an entity (`for b: Box with Main { View.h = b.h }`). Read in the
  same `for`, it is as it was before.
- Another entity: `Hull(target).hp -= damage` (also `+=`, `min=`, `max=`)
  combines into its field when the query ends; reading one may find nothing,
  so it is `if let hp = Hull(target).hp { ... } else { ... }`. So is a
  read through an `optional` binding (`if let x = prev.x`). Several go
  into one `if let` with `, let` between them
  (`if let x = prev.x, let w = prev.w { ... }`): the first block runs
  where all are there.
- `connect (a)-[Synapse { weight: 0.5 }]->(b)` adds an edge from `a` to
  `b` (`connect (a)-[Follows]->(b)` without fields); a node is any
  expression that gives an entity, and arrows can go on, each an edge:
  `connect (side)-[Inside]->(root)<-[Inside]-(rest)`. Outside a `for` it is there
  for the next `for` and everything after; inside one, when the `for` ends
  (at most once per entity; not inside an edge loop). A system that
  connects in a loop outside a `for` sorts the edges once, before its next
  `for` or when it ends, which is also when a tree is found to have a
  cycle. Giving an entity of a tree another parent takes no room in the
  tree's capacity: a tree with as many edges as it may have can still be
  changed.
- `spawn { Position { x: 1.0, y: 0.0 }, Velocity { dx: 2.0, dy: 0.0 } }`
  creates an entity; as an expression it returns its id
  (`let id = spawn { ... }`). An entry of its list may be an `if` that
  says which components it is (`if i == 0 { Gold } else { Lives, Tint {
  rgb: 0xff0000 } }`, with no comma needed after it), or a prefab with
  what it is given (see Prefabs below); a component listed again takes
  the later values. Every way through is a spawn of its own, so which
  components an entity can have is still known when the program is
  compiled.
- `if cond { ... } else if cond { ... } else { ... }`.
- `for i in a..b { ... }` runs its statements with `i` = `a`, `a + 1`, ...
  `b - 1` (not at all if `a >= b`). The bounds are integers of one type,
  which `i` has too; literal bounds take the other bound's type, two
  literals count in `i32`. Loops nest.
- A counted `for` in a system can have `for`s over entities in it, and
  runs them so many times: once for each layer, with the count in the
  filter (`for z in 0..3 { for b: Box, l: Layer where l.z == z { ... } }`),
  or in rounds, where a reactive `for` reacts each time to what has
  changed since the time before, and does nothing in a round that has
  nothing new for it. A system with such a loop is not fused with others.
- An `if` in a system can have `for`s over entities in its branches too,
  which run or do not (`if Inspector.on { for b: Box { ... } }`). A
  reactive `for` there reacts, when it does run, to all that has changed
  since it last did.
- `loop { ... } until cond` runs its statements, and again until the
  condition holds after them (at least once); a var they assign goes
  round with it, and the condition can ask what they named with `let`.
  In a system it may hold `for`s over entities, as a counted `for` may:
  rounds until one changes nothing.
- `while cond { ... }` runs its statements for as long as the condition
  holds before them (not at all, if it does not at first).
- All three loops can stand in the body of a `for` over entities, and
  run there for the entity it is at (`for i in 0..c.n { c.sum += i }`).
  Such a body runs only for the entities that have what the `for`
  binds, where otherwise it may run for all with its writes left out.
- A `for` over entities gives a number where the value of a statement
  is worked out (of a `let`, a `var`, an assignment, the condition of a
  `loop` or a `while`): how many entities its body ran for (an `i32`;
  those a `where` or the triggers left out are not counted).
  `let hit = for h: mut Hull where h.hp < 0.0 { ... }`,
  `total += for ... { }`, `until (for ... { }) == 0`. It runs where it
  stands, before the rest of the value is worked out; not in an `if`
  that gives a value, of which only one branch is. So rounds end
  when no `for` has anything left to do,
  `loop { let placed = for ... { }  let floated = for ... { } } until
  placed + floated == 0` (as `examples/layout.ent` places what floats,
  however deep).

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
  program's to choose. What entities add up, or what one of them has,
  goes into a unique (`+=`, `-=`, `min=`, `max=`, `=`).
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

### Text of any length

```
import text
component Page { words: text }
unique Title: text
fn longer(a: text, b: text) -> text { if len(a) > len(b) { a } else { b } }
```

`text` without a capacity holds as many bytes as it is given. A field or
a unique of it costs eight bytes in its column; the bytes are the
world's, apart from the columns: what is assigned to the field is copied,
and what the field held is given back, as it is when the field's
component is removed or its entity destroyed.

- A literal put where a `text` is expected is one, however long, and a
  `text[N]` goes where a `text` is expected as it is; a `text` where a
  `text[N]` is expected is cut to the bytes that fit (`t as text[N]` in
  an expression, `t as text` the other way).
- `len(t)`, `t[i]`, `a == b` and `a != b` (with a text of either kind)
  are as for a `text[N]`.
- `a + b` with a `text` on either side joins them into a `text`, and
  so does a `text` put into one (`"<{t}>"`); `t += u` joins and stores.
  What joining makes is nobody's: the world keeps it until the schedule
  that runs is done, and a field that is given it takes its copy. (What
  is joined right where a field is given it, `t.words = a + b` or `+=`,
  is made as the field's own: nothing is kept for it.)
- A fn or proc takes and gives them: a literal, a `text` it was given,
  one it joined, or a `text[N]` of its own, which is then kept like a
  joined one. An extern fn or proc
  takes one as `const ent_text *`: a `uint32_t length` and the bytes
  after it, with a 0 after them (which does not count). So does an
  extern system, and a schedule from a C host that runs it (no address:
  a text without bytes).
- When `main` is done, what fields and uniques still hold is given back.
  (A C host's world keeps what it holds.)
- A value of it is what a field holds or a literal is, seen, not a
  copy: a `var` does not hold one, and a `let` does not keep the text of
  a unique or of a `mut` binding, which may be given another while the
  `let` is there. (`t as text[N]` is a copy.)
- In the C header such a column is an array of `const ent_text *` (none:
  no bytes), which C reads and leaves to the program to change.
- The module `text` keeps the bytes (`devices/text.ent`, with its C): a
  module that has a `text` says `import text`. A program run with
  `ENT_TEXT_REPORT` set says at its end how many texts fields held.

## Schedules

```
schedule frame(dt: f32) {
  shoot(dt)
  fly(dt)
}
```

Runs systems in order; arguments may be literals, which take the system
parameter's type. Systems must be declared before the schedules that run
them. A schedule can also run a schedule declared before it, its own or
an imported module's: that one's runs take their place, with its
parameters the values given (`schedule frame() { begin()  ui()
present() }`, where `ui` is a module's schedule of the steps it needs).

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

A `proc` acts: it is called for what it does, by the procs it calls in
turn (a device's, in the end: `rect`, `put`), and may give a value too.

```
proc mark(x: f32, y: f32, w: f32, h: f32, colour: i32) {
  rect(x, y, w, h, colour)
  if w > 40.0 { label("wide", x + 2.0, y + 2.0, 12.0, 0xffffff) }
}
proc both(n: i32) -> i32 {
  put("number {n}")
  n + 1
}
```

- Its body is statements; one that gives a value ends with it, as a fn
  does.
- Like a fn it has nothing of the world: no uniques, no entities. What
  it needs it is given.
- A proc is called where an `extern proc` is: in systems, `world` and
  other procs; not in a fn, which only computes.

### Fns and procs as values

A fn or a proc is a value where a type says what it takes and gives:

```
component Custom { draw: proc(x: f32, y: f32, w: f32, h: f32), data: i32 }
component Transition { seconds: f32, ease: fn(t: f32) -> f32 }

fn ease_out(t: f32) -> f32 { 1.0 - (1.0 - t) * (1.0 - t) }
proc dial(x: f32, y: f32, w: f32, h: f32) { circle(x, y, w, 0x3a7bd5) }

world {
  spawn { Custom { draw: dial, data: 0 },
          Transition { seconds: 0.5, ease: ease_out } }
}
system draw() {
  for b: Box, c: Custom, tr: Transition {
    let paint = c.draw
    let ease = tr.ease
    paint(b.x, b.y, b.w * ease(0.5), b.h)
  }
}
```

- `fn(T, ...) -> U` and `proc(T, ...)` (also `proc(T) -> U`) are types,
  of a field, a parameter or a unique. A parameter may be named in them,
  for the reader. What such a fn gives is a number, a bool, an enum or a
  text.
- The name of a fn or proc is a value where one of its shape is
  expected: its own, an imported module's or an `extern` one. It takes
  and gives exactly what the type says.
- What holds one is called like a function: a name (`let paint =
  c.draw`, a parameter, then `paint(...)`) or the field itself
  (`c.draw(b.x, b.y, b.w, b.h)`, `tr.ease(0.5)`). A proc that is held is
  called where procs are, and not in a fn.
- Two are compared with `==` and `!=` (`if tr.ease == linear`).
- The program is closed: the compiler knows every fn and proc that is
  used as a value anywhere. A value is the number of its function among
  those of its shape, a byte, and a call through it asks which one it is
  and calls that one: no pointers, and the call can be inlined. In the C
  header such a field is an enum of those functions
  (`ent_fn_of_f32_to_f32_ease_out`).
- `none` is no function, where one is expected (`Custom { draw: none
  }`, `if c.draw != none`): calling it does nothing, and gives 0 (no
  text). It is the 0 that memory that was never set holds.

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

## Tables

```
table road { x: f32, y: f32 } = [
  { x: -1.0, y: 2.0 }, { x: 4.0, y: 2.0 }, { x: 4.0, y: 8.0 }
]
table primes: i32 = [2, 3, 5, 7, 11]

let tx = road[w.leg + 1].x
for i in 0..len(primes) { sum += primes[i] }
```

A table is a list of rows that is the program's own and never changes:
with named fields, each row giving a value for every one of them, or of
one type (a plain list).

- `name[i].field` and `name[i]` read a row, in systems and in fns (a
  table is not of the world); where there is no row `i` they give nought
  (`false`, the first case of an enum, an empty text), as a text's byte
  past its end does. `len(name)` is the number of rows, an `i32`.
- `let point = name[i]` gives a row a name, and `point.field` reads it;
  `for point in name { }` goes over the rows in their order. Such a name
  is read by its fields only (a row is not a value that is passed on or
  kept), and never assigned. A row of a plain list is its value (`for
  number in odd { all += number }`).
- A field is a number, a bool, an enum (`kind: Kind.Frost`), a
  `text[N]` or a `text` of any length; a value is written as it is: a
  literal, with `-` before a number.
- A row is `{ field: value, ... }` with every field, in any order.
- It is declared at the top level, before what reads it, and belongs to
  its module like a fn.

A table may have a row for each case of an enum instead of rows that are
counted:

```
enum Kind { Arrow, Cannon, Frost }
table towers[Kind] { cost: i32, reach: f32 } = [
  Arrow:  { cost: 40, reach: 3.2 },
  Cannon: { cost: 70, reach: 2.6 },
  Frost:  { cost: 60, reach: 2.2 }
]
table short[Kind]: i8 = [ Arrow: 'a', Cannon: 'c', Frost: 'f' ]

let c = towers[t.kind].cost
for kind in Kind { all += towers[kind].cost }
```

- Each row is named by its case, in any order, and every case has one,
  once: an enum that gets another case does not compile until its
  tables have a row for it.
- It is read by a value of that enum (`towers[kind]`), not by a number,
  and a table of counted rows not by an enum: no cast, and no row that
  is not there.
- `for name in Enum { ... }` runs its statements for every case of an
  enum, in their order; `name` is the case.

A table is a value where a type says what its rows are:

```
table road_a { x: f32, y: f32 } = [ ... ]
table road_b { x: f32, y: f32 } = [ ... ]
component Level { road: rows { x: f32, y: f32 }, costs: rows[Kind] i32 }

fn road(level: i32) -> rows { x: f32, y: f32 } {
  if level == 0 { road_a } else { road_b }
}
let tx = road(Game.level)[w.leg + 1].x
for i in 0..len(l.road) { ... l.road[i].y ... }
```

- `rows { field: type, ... }` is the type of the tables with such rows,
  `rows type` that of the plain lists of a type, and `rows[Enum] ...`
  that of those with a row for each case. A field, a unique, a parameter
  and a fn's result may have it.
- A table's name is such a value where one of its shape is expected;
  `none` is the table without rows (`len` 0, every read nought).
- What holds one is read like the table: `value[i].field`, `value[i]`,
  `len(value)`, `let row = value[i]`, `for row in value { }`.
- The program is closed, so the value is the number of the table among
  those of its shape that are used as values. The rows of those are also
  kept in one table, one's after another's, and a read through a value
  reads there, from where its table starts: as fast with many such
  tables as with one.

## Prefabs

```
prefab caption(says: text, size: f32, colour: i32) {
  Box { x: 0.0, y: 0.0, w: 0.0, h: 0.0 },
  Size { w: Sizing.Fit, h: Sizing.Fit },
  Stack { row: true, pad: 0.0, gap: 0.0 },
  Text { text: says, size: size, colour: colour }
}

let label = spawn {
  caption("gold", 22.0, 0xf0c85a),
  if i == 0 { GoldLabel } else { LivesLabel }
}
```

A prefab is a named list of what a `spawn` lists, with parameters. A
spawn, or another prefab, names it with a value for each parameter, and
its entries are then the spawn's, as if written there.

- Its entries are components with their values, other prefabs (declared
  before it) and `if`s; their values are worked out from its parameters
  and from what any expression can read where the spawn is.
- What a spawn lists after a prefab comes after its entries: a component
  the prefab has too takes the spawn's values (`caption(...), Size { w:
  Sizing.Grow, h: Sizing.Fit }`).
- It belongs to its module and is found like a fn; its entries name
  things as its own module does.
- `e.add(name(values))`, in a `for`, adds every component the prefab
  comes to for that entity, its `if`s decided there.

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

`device` declarations, optional bindings
(`T?`): each is reported as "not supported yet" where it would start. Of relations, not
yet: joins over relation variables, accumulating into a unique and
connecting inside an edge loop, disconnecting by pair, and of trees what
Trees above lists.
