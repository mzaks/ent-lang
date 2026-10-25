# Indexes

Status: a proposal, built in stages. What is built is said where it
stands, under **Built**; `docs/syntax.md` (Indexes) is the reference for
what the language has.

## 1. Goal

Declared indexes, so that queries find entities by key, by order, by
position and by similarity without scanning every row. The compiler, not
the program, decides as much as it can: how an index is stored, when it
is kept up to date, and which queries use it.

## 2. What there is to build on

- **Relations** are stored as compressed sparse rows (CSR) sorted by
  source, with an index by target where the program goes along incoming
  edges (`ent.relation`, `include/Ent/EntOps.td`).
- **Sorted trees** (`relation ... tree sorted`) keep an archetype's rows
  in breadth-first order (`docs/syntax.md`, Trees;
  `test/Transforms/lower-sorted*.mlir`).
- **Orders by hand in buffers:** `Grid` (`docs/syntax.md`, Buffers), and
  `Order`/`Index` filled by a counting sort with running sums in the
  epidemic (`docs/gpu.md`, `lay` and `pair`).
- **`for ... by field`**: the entities in the order of a whole number,
  by a radix sort of the bytes in which the numbers differ.
- **Lookups** (`ent.lookup`) see other entities as they were when the
  query started (Q3).
- **Access analysis** (`lib/Ent/Transforms/Access.cpp`) knows every
  system's reads and writes, column by column; `--ent-schedule` places
  systems in stages by it.
- `docs/sync-points.md` ("Defined against this model later") has the
  rule for indexes: **they are maintained at commit points, so an index
  is consistent at every query's start and, within a query, reflects the
  state at its start, like lookups.** This spec takes it as it is.

## 3. Principles

1. **As of the start.** A query reads an index as it was when the query
   started. What a query writes reaches the index at the next commit
   point.
2. **Deterministic.** The same program and input give the same contents
   and the same order of an index, on one core, on many and on the GPU.
   Ties go by row order. Seeds (LSH, k-means) are fixed when the program
   is compiled, or set in a unique.
3. **One backend for making them.** Most indexes are rows sorted or put
   in buckets by a key made of fields. One primitive does that: count per
   bucket, an exclusive running sum, scatter; on one core, in parallel
   loops, and on the GPU. The grid, the Morton order, hash buckets, LSH
   and IVF lists are built on it.
4. **An index is derived.** It is never the source of what is true:
   without it the program is slower, never different. Except `unique`,
   which also checks (5.1).
5. **Fixed capacity**, as buffers and relations have. More stops the
   program, saying so.

**Built:** 1 for systems: a system reads an index as it was when the
system started, also where it changes the keys itself. 4: the rows are a
transient buffer, made again from the entities after a world is loaded.
5: an index holds as many rows as its first component's capacity. 2 and
3 not yet: see 5.2 for ties, and step 1 for the backend.

## 4. Keeping an index up to date

How an index is kept is two choices that do not depend on each other:
**when** it is brought up to date, and **how**. Every pair makes sense.

| When | The index is brought up to date | Best where |
|---|---|---|
| `lazy` (the default) | Before the first system that reads it after one that changed it | Readers are rare or conditional: several changes make one update, none where nothing reads |
| `eager` | Right after each system that changes it | It is read soon and often: its cost lands where the change is (staged with what does not read it), readers never wait |

| How | What is done | Best where |
|---|---|---|
| `rebuild` | Every entity listed and sorted anew | Many keys change each time (positions) |
| `incremental` | Only the rows of the entities whose key, or a field it copies, changed: each written and moved up or down to its place | Few keys change (names, ids, cars that overtake) |
| (neither said) | As fits: anew where entities came or went; else every entity writes its row and those out of order are moved down, anew where that would take more steps than there are rows (and then anew without trying the next 1, 2, 4, .. 64 times) | Not known |

```
index Traffic of Car by key { x: Spot.x } eager incremental
```

`incremental` knows which rows changed by the change tracking reactive
queries have already (`on changed C.f`), not a second mechanism: its
update's `for` over the entities is reactive. Where entities came, went
or moved between archetypes (which reactive queries do not see as
changes of the key), and where the rows would move more steps than there
are rows, it is made anew, as `rebuild` does.

A hash index (5.1) has no order to keep: as fits it is mended from the
entities whose key or copies differ from their rows', `incremental` from
those a reactive `for` visits; each moved from its key's list to its new
key's, at the end.

A clustered index (5.2) is brought up to date before every system where
it is out of date, eager or not: its archetype's rows are in its order
whenever a system starts.

**Built** (2026-10-23): both choices, after the key (`index Name of C by
key [lazy | eager] [rebuild | incremental]`), checked against a `for ...
by` the key in `test/Integration/index_ways.test`. `--ent-indexes` runs
the update before every system that reads the index, on the condition
that something changed since it was last made, and, of an `eager` one,
right after every system that changed it (the one before a reader then
finds it up to date). What changed it, it knows from the access
analysis: a system that changes which entities it holds (spawns,
despawns, moves) marks it to be made anew; one that changes only keys or
copied fields, to be made again.

## 5. Kinds of index

The syntax below shows the idea; it is settled in the style of
`docs/syntax.md` when a kind is built. A kind needs `ent.` ops first, the
surface after, as for the rest of the language.

### 5.1 Hash index (equality)

```
index ByName of Player by name hash unique
let p = ByName.get("ada")                  // the entity, or none
for p: Player where p.team == t { ... }    // through `index ByTeam of Player by team hash`
```

Built (2026-10-23, `docs/syntax.md`, Hash indexes):

- Keys: whole numbers, enums, entities and texts (by two hashes of 64
  bits, by two seeds: two texts are taken for one where both are the
  same, a chance of 2^-128; the index does not hold the text).
- Slots of keys (open addressing, a power-of-two table twice the
  entities there may be, at least, linear probing) and a row for each
  entity (`Name_list`, in the order they are stored, with its place
  there): those of a key are a list, from the key's slot, in the order
  they are stored as made. A key many entities have is one slot, and
  its list: one slot an entity would put them all in one run of slots
  (200,000 entities of 7 keys took a second to make so).
- The slots: twice the keys at least (a power of two, 64 at least),
  from twice those of the last time it was made, four times as many
  where the keys come to more than half; made anew where a key more
  would come to that as it is mended. A key's home is the hash's high
  bits (multiply-shift): with the low bits, runs of whole numbers
  landed on half as many homes in a small table (`bench/hash/homes.c`).
- The hash: rapidhash's mix (one 64 by 64 bit multiply, the halves of
  the product xored; `ent.hash`), its seed and secrets fixed. A text
  is hashed 16 bytes at a time in one call (`ent.text.hash`,
  `devices/text.c`). Measured
  against wyhash64 (two such multiplies), aHash without AES (the same
  folded multiply twice and a rotate: its fast form needs the AES
  instructions, which ent's code does not have) and SplitMix64's
  finalizer, in the table as the index has it (`bench/RESULTS.md`,
  2026-10-23): the fastest, by about a fifth, and runs as short where
  keys are at random, shorter where they are made by a rule (numbers in
  a row, multiples of 1,024, high bits).
- A byte for each slot (`Name_tags`): 128 where it is empty, else 7
  bits of its key's hash, the first 16 again after the last. A key is
  looked for as SwissTable looks: the tags of 16 slots from its home
  compared at once with its own and with empty (one load, two compares
  into masks), its key read only where the tag is its own and before
  the first empty slot, the next 16 only where there was none
  (`ent.index.get`, and the `for`s it answers).
- `Name.get(key)`: the entity of a key, the first as they are stored
  where several have it; `none` where none has it. Lowered where it is
  (`ent.index.get`): a key one entity has gives the slot's own entity
  (`one`), read beside its key; others go along the key's list. Against
  absl::flat_hash_map by the same loop (`bench/hash/get.py` and
  `swiss.cpp`): keys not there faster from 200,000 keys on (3.4 ns
  where 5.8, at 4 million 4.0 where 9.5), keys found 1 to 1.5 ns
  slower in the cache and as fast beyond it (35 ns where 33).
- `for x: C where x.key == value` (or `value == x.key`, or one of the
  conditions `&&` joins), where the value is the same for all its
  entities (literals, consts, enum cases, locals not declared `var`):
  the value is worked out before the `for`, and `--ent-indexes` marks
  it to visit only the entities the index says have it (`via_hash`),
  in the order they are stored; the `where` is still asked. Where the
  index holds the same entities, is kept up to date before the system,
  and nothing in the system before the `for` (or in it, of other
  entities) changes the key or which entities there are. Where one in
  64 of the entities or more have the key, the `for` looks at all of
  them, as without the index, a loop that costs less than finding each
  by its id; which, it says from the key's slot as it runs.
- `unique`: where two entities have one key, when the index is made
  (or mended), the program stops, naming the key and both entities.
- Kept each of the ways of section 4: `rebuild` makes it anew; as fits
  and `incremental` mend it, from the entities whose key or copies
  differ from their rows' (as fits, all looked at) or that a reactive
  `for` says changed (`incremental`, through its log: the list of what
  changed is in any order, `ent.any_order`): each taken out of its key's list
  and put at the end of its new key's (a key none has now: its slot
  emptied, those after it moved back, no tombstones).
- For: an entity by name or an id from outside, a netlist's signal name
  to its entity (AIGER and SPICE import), network ids.

### 5.2 Sorted index (order, ranges, top-k)

```
index Traffic of Car by key { x: Spot.x, v: Speed.v }
```

- **Secondary:** the entities with the index's components in the order
  of a key, apart from where they are stored, so that an archetype can
  have several orders, which sorted trees cannot, and an order can go
  over several archetypes.
- **Clustered (optional):** the archetype's rows themselves in the
  key's order, as `tree sorted` keeps them. One clustered order an
  archetype; the compiler rejects two.
- Made by a radix sort, for whole-number keys and floats (by the usual
  flip of the sign bit). Ties in row order.
- For: queues of events, top-k, VSIDS-like orders in the SAT solver,
  leaderboards, the car ahead and the cars beside it
  (`examples/traffic/lanes.ent`).

**Built** (secondary, 2026-10-22; `docs/syntax.md`, Indexes):

- `index Name of Component, .. by field { name: Component.field, .. }`:
  a row for each entity, with its key (`key`), the entity (`who`) and
  the fields it copies, which a scan of the rows reads without looking
  up the entities. Each entity keeps its row in a field of the first
  component (`row_in_Name`): `Name.row(e)` is one lookup.
- Read as a buffer that only systems read (`Name[r].x`, `len(Name)`,
  handed to a fn), with `Name.find(key)`, the first row not below a key.
  A `for ... by` the same field over the same entities takes its order
  from it (2026-10-23, below).
- Ties: in the order the entities are stored, however it was made
  (2026-10-23; before, made again in place, as they were in it): each
  row has its entity's place (`place`), counted when the index is made,
  and the insertion goes by key and then by place. The same as a `for
  ... by` the key gives (`test/Integration/index_ties.test`).
- Keys are whole numbers or fractions (`f32`, `f64`, 2026-10-23), as for
  `for ... by`: a fraction's bits, those but the sign's turned over
  below nought, nought below nought as nought (`test/Integration/
  float_keys.test`).
- **Query forms** (built 2026-10-23, `test/Integration/by_slices.test`):
  `for c: Car by c.key` takes its order from an index by `Car.key` that
  holds the same entities, instead of sorting them (`--ent-indexes` marks
  the `for`, `by_index`, and counts it as reading the index, so that it
  is made before); with `from a to b` (a <= key < b), `first k`, `last k`
  after it, a slice of the order, by halving the rows (with an index or
  sorted). Without an index the same entities in the same order: the
  index changes only how fast (principle 4). Only where the program reads
  the index itself (else it would be made for the `for` alone, which
  costs more than the sort: bench/RESULTS.md, 2026-10-23); not where the
  system changes the key or which entities there are before the `for`,
  nor on the GPU, where the slice is found after the GPU's sort.
- Made again in place, an index stops moving rows down past those above
  them once that has taken more steps than there are rows, and is made
  anew (keys changed at random: 200,000 entities, one in 50 changed,
  163 ms to 2.6 ms on one core).
- **Clustered** (built 2026-10-23, `test/Integration/clustered.test`):
  `index Name of C by key clustered` keeps the rows of the one archetype
  that holds its entities in its order whenever a system starts:
  `--ent-indexes` makes it before every run where it is out of date, and
  its update ends in `ent.cluster`, which moves the rows into the index's
  order (every column and the ids, the entity table following, as a
  sorted tree moves them) and makes each row's place its row. An
  archetype a sorted tree keeps, entities of several archetypes, or two
  clustered indexes of one archetype: errors. Another index over the
  archetype is made again as the rows move (clustered ones are placed
  first).
- `examples/traffic/lanes.ent` uses it: to the bit what the C version
  gives, as fast as the order it kept by hand, and its update six times
  cheaper made again in place (`bench/traffic/RESULTS.md`).

### 5.3 B+ tree (large, slowly changing, ordered)

- The queries of 5.2, kept `incremental` with O(log n) updates.
- Nodes in a node pool of fixed capacity, a buffer, 8 to 16 keys a node
  (one or two cache lines); leaves linked by `index of` for ranges.
- One writer, at the commit point: no latches.
- Worth it only for large sets of keys (100,000 and more) of which few
  change a frame; else the sorted index wins. After 5.2, measured
  against it.

### 5.4 Partitions by value (fields of few values)

```
index Unit.state partition      // state: an enum
for u in Unit where state == .attacking { ... }
```

Two ways; the compiler picks one, or the program says which:

- **In the archetype:** the value is a hidden tag of the archetype, so a
  filter by it is free, but every change of it moves the entity (as
  `apart` tags are kept).
- **A bitmap or a list of rows for each value:** cheap to change, a scan
  of the list to read.

For: alive and dead, the states of units, satisfied and unsatisfied SAT
clauses, resting and refractory neurons.

### 5.5 Spatial indexes

All take a position (2D or 3D, `f32` or whole numbers) and answer at
least `within r of p` and `in box`; k-d trees also `nearest k`.

```
index Unit.pos grid cell 4.0 capacity 100000
index Star.pos morton bits 21 capacity 10000000
index Site.pos kdtree capacity 50000
for o in Unit near u.pos within 3.0 { ... }
for s in Site nearest 5 to p { ... }
```

| Kind | Made by | For |
|---|---|---|
| **Uniform grid** | Cell = floor(pos / cell size). A counting sort by cell, then a running sum gives each cell's start (the `lay` pattern) | Objects of a similar size, spread evenly: particles, boids, crowds, the broad phase of collisions |
| **Linear quadtree or octree (Morton)** | Key = the bits of the quantized coordinates interleaved (Z order), radix sorted. A cell is a run of rows that share a prefix, found by halving | Uneven density, Barnes–Hut, levels of detail, sparse voxels. The general default |
| **k-d tree** | Median splits, kept in breadth-first order (children at fixed places, as sorted trees are). Made again at the commit point | Nearest k of points that mostly stay |
| **BVH (later)** | Over boxes, not points, made from the Morton order (LBVH) | Objects with extent: rays, line of sight, collisions of meshes and boxes |

- Morton: the bits spread by shifts and masks (or `pdep`, x86 BMI2). A
  query for neighbours looks at the cells next to it too, since the Z
  curve jumps at the borders of quadrants. Hilbert later, perhaps.
- Perhaps: the archetype's rows clustered by the Morton key, so that
  neighbours are near in memory too. Measured on what reads neighbours
  (`swarm`, `boxes`).
- Not a quadtree of pointers made of a `tree` relation with cells as
  entities: cells that split and merge are spawns and despawns every
  frame.
- A k-d tree's queries need a small stack: a buffer, or a fn that calls
  itself.

### 5.6 Relation indexes

What relations have, extended:

- An index by source and one by target for every relation whose edges
  queries read either way (today: by target only where edges into an
  entity are visited).
- Indexes on edge fields: edges sorted or in buckets by a field (the
  heaviest `weight` first, say).

For: fan-in and fan-out in `examples/logic`, variables to clauses and
back in the SAT solver, synapses by their neurons before and after.

Built (2026-10-24; bench/graph, against the GAP Benchmark Suite):

- Read both ways, the table is by source and an index of positions by
  target (as before); its sort now also leaves the edges' ends and the
  fields no loop writes in the index's order (in the scratch columns), so
  a loop into an entity reads them in turn, as GAP's graph transposed:
  PageRank 151 -> 45 ms at 2^18 (GAP 41).
- Kept in order in place where few edges change (devices/edges.c): runs
  with room, the order a sort gives, sorted anew past a sixteenth;
  rewiring 1 to 10,000 of 7.6 million links costs 0.6 to 11 ms a step
  instead of a sort (60 to 75 ms).
- Indexes on edge fields: `index Name of R by field` and `by source,
  target`, read by `for (e)-[s: R]->(v) by s.field [from a] [to b]
  [first k | last k]` and `where v == b`, which work without an index
  too (see docs/syntax.md, Indexes of a relation). The three heaviest
  links of every page: 2.3 ms with the index, 52 without. An index by
  the ends pays only for entities of many edges: on GAP's graph (29 a
  page) going over them is as fast.

### 5.7 Similarity: locality-sensitive hashing (LSH)

Hash functions under which similar entries collide.

| Similarity | Family | Hash |
|---|---|---|
| Euclidean | Random projection | `floor((a·v + b) / w)`, `a` Gaussian, `b` in `[0, w)` |
| Cosine, angle | SimHash | A bit for each random hyperplane: `sign(a·v)` |
| Jaccard (sets) | MinHash | For each of k hash functions, the least over the set's elements |
| Hamming (bits) | Bit sampling | A fixed random subset of the bits |

- Bands: k hashes together are one bucket's key, in L tables. A
  candidate is an entity that collides in one table at least; then the
  real distance is checked. k and L are parameters.
- Each table is a hash index of buckets (5.1's storage, the backend of
  section 3). The L tables are made in parallel.
- Projections and seeds fixed (principle 2).
- For: states seen before (cycles in the logic simulator), equivalent
  subcircuits, duplicate SAT clauses and candidates for subsumption,
  agents that behave alike.

### 5.8 Embeddings and similarity of meaning (ANN)

Vectors of 384 to 1536 numbers, normalized where they come in, so that
cosine similarity is a dot product. Making the embeddings is **not part
of this**: a device or an `extern system` does it (ONNX Runtime,
llama.cpp); ent-lang keeps them and indexes them.

In this order:

1. **Brute force** over int8 or binary vectors (binary: the sign bits,
   compared by popcount), the best candidates ranked again with the full
   `f32` vectors. One query loop; fits the GPU. Also what the recall of
   all that follows is measured against.
2. **IVF**, in ent-lang: k-means, each step two passes. Each vector
   finds the nearest centroid; then adds itself into that centroid's
   sums (`+=` into another entity, the same in parallel). The lists of
   the clusters by the backend of section 3. A query searches the
   `nprobe` nearest clusters. Product quantization of the codes, perhaps.
3. **HNSW**, only from outside, behind an `extern system` (hnswlib or
   FAISS): its inserts go one after another and depend on their order,
   which does not go with indexes that are made deterministically at
   commit points. Not built in ent-lang.

For: search by meaning and retrieval for RAG, recommendations, what an
agent or an NPC remembers, queries in plain language matched to
entities.

## 6. Semantics to say exactly

For each kind, `docs/syntax.md` says:

- What a query sees of an entity spawned, despawned or changed earlier
  in the same frame (rule: what was committed before the query started).
- In what order the results come (rule: by key, ties in row order; in
  hash and LSH buckets, row order).
- What happens beyond the capacity and where `unique` does not hold.
- What `gpu system`s may read (an index is read only in a kernel; it
  may itself be made on the GPU).
- How `--ent-schedule` treats an index: making it is a run that reads
  the key columns and writes the index, so what reads the index goes in
  a later stage.

**Built:** for the sorted index the first, the third (its capacity is
the component's: no row more than there are entities) and the last (the
update is a system like any other, run where `--ent-indexes` puts it,
and staged by `--ent-schedule`). Order of ties: see 5.2. GPU: not yet.

## 7. Compiler passes

1. **Ops**: `ent.index` (the declaration: kind, key fields, capacity,
   options, perhaps the policy) and ops or attributes for queries that
   use one. The verifier checks that the key fields are there and of
   types that can be keys, one clustered order an archetype, partitions
   only of enums and small whole numbers.
2. **`--ent-lower-indexes`** (or part of `--ent-lower-to-loops`): the
   index's storage in the arena and the code that makes it and keeps it,
   on the shared primitive.
3. **Access**: making an index shows in `--ent-print-access`, and
   `--ent-schedule` puts it in the earliest stage after the last writer
   of its key fields.
4. **`--ent-infer-indexes`** (the part that is ent-lang's own, last),
   by the access analysis:
   - the policy, from how often the key fields are written;
   - declared indexes no query reads dropped, with a remark;
   - indexes for queries that scan, suggested (equality: hash; ranges
     and order: sorted; `near`: grid or Morton), as remarks first, made
     by themselves only behind a flag.

**Built**, otherwise: no `ent.index` op. The parser declares a buffer
with an `ent.index` attribute (what it holds and is made of), a field
for each entity's row, and, as ent-lang, the systems and the unique that
make it and say whether it is up to date; `--ent-indexes` (first in
`tools/ent`'s pipelines) places their runs by the access analysis
(section 4). The update shows in `--ent-print-access` as the system it
is.

## 8. Order of the work

Each step comes with lit and FileCheck tests (`test/Dialect`,
`test/Transforms`), a test in `test/Integration`, its docs, and a
benchmark in `bench/` with its numbers in a `RESULTS.md`.

| Step | Work | Shows | Status |
|---|---|---|---|
| 1 | The shared sort-and-scan primitive (one core, parallel loops, GPU). `lay`'s order by hand ported to it, the same to the bit | The backend | built: `for ... by` listed, sorted and (giving rows) visited on many threads and on the GPU (see below); one radix sort for one core and many (2026-10-23) |
| 2 | Uniform grid and `near ... within` (a Lennard-Jones melt as its example) | `rebuild`, scheduling of makes | built (see below), and `after me` (2b); grids of whole numbers, `between .. and ..`, `nearest k to` (2026-10-23) |
| 3 | Hash index and `unique` | `incremental`, change tracking | built: `by key hash [unique]`, `get`, `where x.key == value` through it, every way of section 4 (2026-10-23) |
| 4 | Sorted index (secondary), then clustered | Ordered iteration, ranges, top-k | built: secondary (5.2); ties, float keys, query forms (`by` with `from`, `to`, `first`, `last`) and `clustered` (2026-10-23) |
| 5 | Morton linear quadtree and octree | Keys made of fields, ranges of prefixes | designed (2026-10-25, below): `morton`, `bits`, `leaf`, `cells { }` sums, `for c in Name.cells` with `open`; references PBBS `octTree` (`bench/points`) and Barnes's treecode |
| 6 | Relation indexes by source and target everywhere, indexes on edge fields | | built: copies in the index's order, kept in order in place, `by` and `where` on edge loops and `index .. of Relation` (2026-10-24, see 5.6) |
| 7 | Partitions by value | | |
| 8 | k-d tree and `nearest k` | | `nearest k` built on grids (2026-10-23); k-d tree to do |
| 9 | Embeddings by brute force (int8 and binary, ranked again) | The recall to measure against | needs a vector type |
| 10 | IVF with k-means in ent-lang | | |
| 11 | LSH families | | |
| 12 | `--ent-infer-indexes` | | |
| 13 | B+ tree, BVH, HNSW from outside (where measurements or programs need them) | | |

**Step 1, as it stands** (2026-10-22). The sort that every `for ...
by` does (`ent.ordered.sort`, a stable radix sort of the keys' bytes)
runs on many threads where the program is built with `--parallel`, from
16,384 entities on: the keys in 64 pieces, each piece counting its keys
by a byte on a thread of its own, a running sum of the counts, each
piece putting its keys where those say; the bytes in which no key
differs from the first are left out (an or over all of them, on many
threads). The order is the one sort on one core gives
(`test/Integration/ordered_parallel.test`). An index made anew sorts
so.

- `lay` is not a sort: each person takes the next stubs, a running sum,
  which `for`s already do on many threads and on the GPU. Nothing of it
  is to port; the grid (step 2) is where counting by bucket and a running
  sum come together, and that is what to build on this.
- The keys are listed on many threads too, each archetype's rows at
  their place. And a body that does nothing but give rows to buffers
  (each `+=` once for each entity, in the body itself) and read is
  visited on many threads: the buffers are given all the rows first, and
  each entity writes its own at its place in the order times the rows it
  gives, after those the buffer had. So an index made anew, whose `for
  ... by` is such a body.
- A million entities by keys at random, 12 threads: 5.9 ms a `for ...
  by` where it is 11.1 on one core (10.0 with only the sort on many
  threads); 0.84 ms of it on one thread (the scans between the passes,
  and what is allocated). 120,000: 0.70 ms where 0.84.
- The GPU (2026-10-23): a `for ... by` that only gives rows runs in a
  `gpu system`, its entities listed, sorted by a radix sort of the GPU's
  and visited there (docs/gpu.md, "A `for ... by` on the GPU"); rows
  given to buffers by other `for`s too, in the order of the entities.

**Step 2, as it stands** (2026-10-22). `index Name of C grid (x, y, z)
cell w [wrap size] { .. }` (`docs/syntax.md`, Grids) is a sorted index
whose key is the cell, a field the index keeps in the entity: the same
making anew (the parallel `for ... by`) and in place (each entity its
row, insertion), and a table of each cell's first row. The sizes are
worked out where it is made, of uniques. `for e, b: C near (x, y, z)
within r in Name` (in a `for`) goes over the cells around the place,
as many out as `r` reaches, round the box where it wraps, `b`'s place
seen as its nearest image: the same as a scan with that distance in
its `where` (`test/Integration/near.test`).

- `examples/lj`: LAMMPS's LJ melt, 32,000 atoms. At step 0 the energy an
  atom (-6.7733681) and the pressure are those of the lattice, and the
  total energy of the potential shifted to 0 at the cut stays within
  5e-5 over 100 steps (`check/melt.py`); to the bit what the same in
  plain C gives (`test/Integration/lj_melt.test`). 100 steps on one core
  4.9 s where the C takes 4.25; on 12 threads 0.76 s.
- The policy is still `lazy`: made before `forces` reads it, after
  `kick_drift` moved the atoms, every step (0.6 ms of a step of 48).
- Built 2026-10-23: a grid of whole numbers (`near_whole.test`), a
  range as `between (x0, y0) and (x1, y1)` (`between.test`) and
  `nearest k to (x, y)` (`nearest.test`), of any grid, in
  `docs/syntax.md`. `nearest` looks within a distance from how many
  entities the grid holds for its size (k and a half of them, were they
  as many everywhere), twice that where fewer were, and keeps the
  nearest k in order in a list on the heap: on one core, of 100,000 dots
  in 2D and 8 nearest each, about twice what `near` takes within the
  distance that has 9 (measured on battery, by itself).

**Step 2b, each pair once: `after me`** (built 2026-10-22, as proposed
below; `test/Integration/near_after.test`. The half melt on one core
0.96 s where 1.19, its count and fill 0.7 and 1.9 ms a step where 1.4
and 3.5; on 12 threads 0.31 s where 0.35). A half
list of neighbours (each pair once, the other given the opposite force)
says which of the two takes the pair. `examples/lj` says it by number,
`where o != me && b.number > a.number`, and so goes over all the atoms
around and drops half of them: the same scan as a full list, half the
pairs. LAMMPS goes over half of what is around (its half stencil): its
lists take 1.2 ms a step where `melt.ent`'s take 5.3.

The surface: `after` and the entity the `for` around visits, after the
index's name.

```
for me, a: Atom, l: mut List {
  var n: i32 = 0
  for o, b: Atom near (a.x, a.y, a.z) within reach in Cells after me {
    n += 1
  }
  l.count = n
}
```

- It visits what the `near` without it visits whose row in the index is
  after `me`'s (`row_in_Cells`): the same entities in the same order as
  `where b.row_in_Cells > a.row_in_Cells`, so never `me` itself. Where
  the `for` around visits every entity of the index and the place is
  each one's own, every pair within `r` is visited once, from the one of
  the two whose row is first. Which one that is depends on the cell
  size and the order the index keeps, as the order of the visits does
  already; the same on one core and on many.
- `me` is the entity the `for` around visits, by its name, and that
  `for` binds the index's first component (so it has a row); else it is
  an error. A `where` after it as before (`after me where b.kind == ..`).
- The compiler: the rows `near` goes along are runs, each from a first
  row to a last (see step 2); with `after me` each run starts at the
  row after `me`'s where it starts before it, and a run that ends before
  it is left out. The cells the rows are in are in the order of their
  keys, so the cells before `me`'s give nothing, as do the rows of its
  own cell before its row. Nothing about the cells is assumed: the
  place may be anywhere, the box less than a cell or two wide. About
  half the rows, without testing them; with the cells of
  `examples/lj` (3 cells of 3.05 across the 2.8 it reaches) about 220
  where 430 now.
- Which atom takes the pair is not even. Of a box that wraps, the cells
  just across its last side are first in the order: an atom in the
  first layer of cells takes the pairs of the last layer too, and one in
  the last layer none of the first. On average half; on many threads the
  slices are of the archetype's rows, each across all the cells, so
  about even too (to measure).

Tests: `test/Integration/near.test` with `after me` against the same
`near` with `where b.row_in_Cells > a.row_in_Cells`, in a box that wraps
and one that does not, `r` within a cell and beyond, a box of fewer
cells than `r` reaches; and that every pair comes once (twice the
pairs of `after me` are those of `where o != me`). The errors, in a
`test/Import` test. `lj_melt.test` against the C as now.

`examples/lj` then: a half list takes `after me` in place of the number
(its own `for` near, beside the full list's `where o != me`, as
`Melt.half` is a value of the world, not of the source), in the count
and the fill; `check/melt.c` the same by the index's rows, to the bit.
What I expect: the count and the fill of half lists about half (1.4 and
3.5 ms a step to about 0.8 and 1.9), the half melt 1.19 s on one core
to about 1.0, the force loop as it is.

Not chosen:

- A half by the cells around, as LAMMPS has it (the cells ahead of one's
  own, and in its own the rows after): each atom takes as many as
  another. But "ahead" is of the cells as seen around the place, which
  are not one order where the box is less than 2m + 1 cells across
  (`test/Integration/lj_melt.test` has 3), and it ties the meaning of a
  program to the grid's geometry more than an order of rows does.
- The compiler finding `where b.row_in_Cells > a.row_in_Cells` itself:
  the same, but by the name of a field the index keeps for itself.
- `where b.number > a.number`, as now: it says the most (a pair belongs
  to the lower number, whatever the index), and goes over twice the
  rows. It stays as it is for what needs that.

Decided (2026-10-22): `after me`; a program that chooses between a half
and a full list by a value of the world keeps two loops.

**Step 5, as designed** (2026-10-25, the user's choices). A Morton
index: the grid's queries over cells that split where the entities are
many, and a walk over those cells with sums each keeps (Barnes–Hut,
levels of detail).

```
component Star { x: f64, y: f64, z: f64, m: f64 } capacity 1048576
component Force { ax: f64, ay: f64, az: f64 } capacity 1048576
unique Sim { theta2: f64, eps2: f64 }

index Stars of Star morton (x, y, z) leaf 16 { m: Star.m }
  cells { m: sum Star.m, mx: sum Star.m * Star.x, my: sum Star.m * Star.y,
          mz: sum Star.m * Star.z }

system gravity() {
  for me, a: Star, f: mut Force {
    var ax: f64 = 0.0
    for c in Stars.cells {
      let dx = c.mx / c.m - a.x
      let dy = c.my / c.m - a.y
      let dz = c.mz / c.m - a.z
      let d2 = dx * dx + dy * dy + dz * dz
      if !c.leaf && c.size * c.size > Sim.theta2 * d2 { open }
      if c.leaf {
        for o, b: Star in c where o != me { .. each pair .. }
      } else {
        .. the cell as one body of mass c.m at its centre of mass ..
      }
    }
    f.ax = ax
  }
}
```

The declaration:

- `index Name of C morton (x, y)` or `(x, y, z)`: fields of the first
  component, of one type (f32, f64, i32 or i64), as a grid's. After it,
  in any order:
  - `bits N`: how finely each side is cut, 2^N steps. At most 31 a side
    in 2D and 21 in 3D (a key of 63 bits at most, so it is in order as an
    i64), which is the default.
  - `leaf N`: a cell of at most N entities is not split. The default is
    16. A cell at the finest level, 2^-bits of the box, is a leaf
    however many it holds.
  - `wrap size`: a box from 0 to `size` on every side that wraps, as a
    grid's.
  - `{ name: C.field, .. }`: what each row copies, as other indexes.
  - `cells { name: sum e | min e | max e, .. }`: what each cell keeps.
    `e` is an expression of the components of the index's entities
    (`Star.m * Star.x`), and the value is of its type.
  - `lazy`/`eager`, `clustered`: as other indexes. Not `incremental`
    (the keys are worked out for every entity each time, as a grid's).
- The key: each place quantized over the box (the lowest corner where
  the entities are, the widest side of them, so the cells are cubes; or
  the wrapping box), the bits interleaved, x the lowest of each level.
  The rows are in the order of the keys, ties by the order the program
  keeps the entities in.
- A cell is a run of rows whose keys share their first `level` digits of
  `dims` bits, the root holding all of them. Its cells are those of the
  next level that have rows. A cell with one cell inside is kept as it
  is (the levels go one by one, as Barnes's tree).
- Each cell has, besides what `cells { }` says: `count`, `size` (the
  side of its cube), `x`, `y` and `z` (its cube's centre, by the names
  of the index's fields), `level` (the root's is 0), `leaf`, and `first`
  (the first row; `count` rows from it). Sums are added in the order of
  the rows, so they are the same on one core and on many.

The queries:

- `near .. within r in Name`, `between .. and .. in Name`, `nearest k to
  .. in Name`, `after me`: as of a grid, the same entities, nearest
  first where `nearest`. `near` and `between` visit in the order of the
  rows (the Morton order). A program changes from a grid to a Morton
  index by its declaration.

The walk:

- `for c in Name.cells { .. }` visits the cells depth first from the
  root, those inside a cell in the order of their keys. `open` (a
  statement, as `continue`) skips the rest of the body and visits the
  cells inside `c` next. A cell that is not opened has none of its
  cells visited. Opening a leaf visits nothing more.
- `continue` skips the rest of the body without opening, and `break`
  ends the walk.
- `for o, b: C in c where ..` visits the entities of cell `c`, any cell,
  in the order of their rows: what the index copies is read from the row
  and the rest is looked up, as `near`.
- It may be inside a `for` over entities, one walk each (on many
  threads, across the entities), and outside one. The fields of `c` are
  read only, and `c` is not handed on.

Upkeep, as a grid's: made anew where entities came or went; where only
places changed, each entity writes its key into its row and the rows
out of order go down to their place, or are sorted anew where many
moved. The cells and their sums are made again each time, from the rows,
in a pass from the leaves up (on many threads by subtree).

Not in this step: the GPU; an `incremental` index; a cell with one cell
inside skipped (a compressed tree); Hilbert order.

**Part 1, as built** (2026-10-25): the declaration (`bits`, `leaf`,
`wrap`, `{ }`, lazy/eager, `clustered`) and the grid's queries; the
cells, their sums and the walk are part 2. How it is laid out and asked:

- The cells (`Name_cells`) are laid out a level after the other, not in
  preorder: a cell's cells side by side (`child` the first, `kids` how
  many), so that the nearest of them can be chosen among them; `next`
  is the first cell after all those inside it, depth first, which a walk
  without a stack goes on by. Each has the tight box of its entities
  (not its cube), worked out from the last cell back.
- `near` and `between` walk without a stack: into a cell (`child`)
  whose box the query meets, else on (`next`).
- `nearest k` first takes the k rows on either side of the place's own
  key into its list, which bounds how far it looks from the start; then
  a stack of cells, each with how far its box is, a cell's cells put in
  so that the nearest is on top, and none farther than the list's k-th. A walk without
  a stack in the order of the keys was 1.5 times as slow on the cube.
- `clustered` is what makes it fast: the queries come in the order of
  the rows, so a query's cells and rows are those the one before read.
  Without it, on a million points the cells and rows of each query are
  misses (2.5 s, against 1 s clustered).

The references:

- The queries: PBBS `nearestNeighbors` and its `octTree`
  (`bench/points`). The aim is to be within octTree's time on the
  clustered points too, where the grid is 82 to 450 times it.
- The walk: Barnes's treecode 1.4 (NEMO's copy, `src/nbody/evolve/
  hackcode/treecode1`), its Plummer model, `theta` and `usequad`, ported
  as an example. Forces against a direct sum: the same error as
  treecode's at the same `theta`, and with `leaf 1` and treecode's root
  cube, the same cells and the same forces as treecode's, to the bit if
  the sums go in its order.

Tests: the queries of a Morton index against those of a grid on the same
entities (`near`, `between`, `nearest`, `after me`, a box that wraps,
whole numbers), on one core and on all; a walk that opens every cell
against a scan (the sums of every cell, the leaves' entities); a walk
that stops at a level; the errors (in a `test/Import` test).

## 9. When it is done

- Every query gives the same with an index as without (an index changes
  only how fast), checked by running each test both ways. (The sorted
  index as built is read explicitly, `Name[r]` and `Name.find`, and by
  `for ... by` its key; a hash index by `where x.key == value`, checked
  so in `test/Integration/hash_index.test`.)
- The same on one core, on many and on the GPU.
- Each kind has a benchmark that shows where it beats a scan and where
  it does not (the B+ tree against sorting again when many keys change).
- ANN: recall@10 against brute force, for IVF at several `nprobe`.

## 10. Not part of this

- Making embeddings (a model outside, by a device or an `extern system`).
- Indexes written to inside a query, by several at once.
- Indexes in a saved world: they are made again where it is loaded.

## 11. Open questions

1. Syntax: a declaration of its own, or options on the component
   (`component Player { name: text @hash unique }`)? **A declaration of
   its own** (`index Name of ...`), as built.
2. Should the compiler ever add an index by itself, or only suggest one?
3. May a clustered order conflict with a `tree sorted` order, one of
   them winning, or is that an error? (Two sorted trees with their
   entities in one archetype are an error.)
4. A vector type (`f32[384]`) for a field: there is none (the one type
   of a fixed size is `text[N]`). Step 9 needs it first.
