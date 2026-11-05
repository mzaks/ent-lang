# 09 · Indexes as a language feature, and a SAT solver

*16 to 26 October 2026. The SAT solver came first, from 16 October, and is
where the first index-like type came from. The indexes proper (hash, sorted,
grid, relation, Morton, k-d tree, partition, and then inferring them) came
between 23 and 26 October.*

## Language or device?

I went back and forth on this one. An index is a data structure, and data
structures usually live in libraries. In ent-lang that would make it a
*device*, behind `extern fn`. I decided it belongs in the language for one
reason: **the problem space**.

ent-lang is for simulations. Simulations ask "who's near me", "who's next in
line", "which of these is in stage X", every tick, for every entity. If the
index is a device, the compiler can't see it. It can't keep the index up to
date at the right moment, it can't make it on the GPU, and it can't turn your
plain loop into an index lookup. If it's in the language, it can do all three.

The rule came from the sync-point model: **a query reads an index as it was
when the query started**, the same as lookups. What a query writes reaches the
index at the next commit point. That one sentence made every index below safe
to use from parallel loops.

## The stress test first: entsat

On 16 October we started a SAT solver in ent-lang. Partly for fun, mostly
because a CDCL solver is the opposite of what ECS is good at: one long
sequential search, pointer-chasing through watch lists, nothing to vectorise.
If ent-lang could do that without embarrassment, the language was more general
than its examples.

The phase gates were real ones:

- all 4,999 SATLIB uf/uuf problems (50 to 250 variables) answered correctly;
- every UNSAT answer's DRAT proof verified by drat-trim;
- then 100 problems sampled from the SAT Competition 2023 main track,
  300 s each, against MiniSat 2.2.

Phase 1 solved 26 (MiniSat 28). By the end of phase 2 its PAR-2 was already
below MiniSat's (0.93×). On the final dev set of 39 problems it solved 38, at a PAR-2 of 2,425,
with a median time of 0.55× MiniSat's on the problems both solve. Along the way
it gained what modern solvers have: Glucose restarts, a stable mode, target
phases, tiered reduction, Kissat's reason bumping, and CaDiCaL 3's reduce
schedule.

What it taught the *language* was more interesting than the solver:

- **`Name = Name[..n]`** keeps the first rows of a buffer, which an arena needs.
  I picked that spelling over `len(Name) = n` and `truncate(Name, n)`.
- **`index of Arena`** is typed row numbers into a buffer. Every buffer read
  used to check the row count, and the propagation loop paid for it. We
  measured first: a fully unchecked build was 11.6% fewer cycles. With
  `index of`, reads are masked to a power-of-two capacity instead, and the
  width (8, 16 or 32 bits) follows from that capacity. That got −10.0%, with
  the type system kept honest. One note: a read below capacity but past the
  length returns a stale row. I allowed that *for now*, and it's flagged to
  revisit.
- **Memory you get back.** One competition problem ran a 22 GB machine out of
  memory during preprocessing. It was strengthening clauses by copying them, one
  literal at a time: 3.45 GB that was never returned. Strengthening in place took
  214 MB. Lesson learned: run benchmarks in a `MemoryMax` cgroup, so the OOM
  killer takes the benchmark and not the terminal Claude is running in.

## Hash: `get` and `where`

```
index ById of Ship by id hash unique
```

- `ById.get(k)` gives the entity, or none.
- A plain `for s: Ship where s.id == k` uses the index **with the same results
  in the same order**. Which way it runs is a decision made at run time: a
  plain scan when the list is long, a walk of the list when it's short.

The design started as a slot per entity and became a slot per *key*, with
linked rows in storage order. The per-entity version was quadratic for few
keys: seven keys over 200k entities took more than a second to build. Benchmarks
picked the parts: rapidhash's mixer beat wyhash, aHash's fallback and SplitMix64
by about 20%. A key's home slot is the hash's *high* bits, because the low bits
of that mixer clump runs of integers. Probing uses 16-byte SIMD tags, as
SwissTable does.

Against abseil's SwissTable in the same loop: misses are faster from 200k keys
on, and found keys are about 1 ns slower in cache and equal beyond it. In the
benchmark program, a read through the index took 2.8 µs where the scan took
16.6 µs. Keeping the index up to date costs 0.1 to 0.24 ms a frame.

## Sorted: `by`, and the queries hiding in it

```
for w: Walker by w.met last 10 { ... }
```

`for ... by` already existed as a radix sort on the spot. With
`index Name of C by key`, the same `for` reads the index instead: same rows,
same order. Then `from a to b`, `first k` and `last k` came along, which also
work without an index. Ties are kept in storage order. Float keys sort by
order-preserving bits, so −0.0 equals 0.0, on the GPU too.

Two findings worth writing down:

- The compiler only uses the index for a `by` if the program *also* reads the
  index itself. An index made just for one `for ... by` was slower than
  sorting: 3.3 ms against 1.2.
- Mending an index in place was quadratic when keys changed at random: 163 ms.
  A step budget, which makes the index anew when mending would take more steps
  than there are rows, brought it to 2.6 ms.

Upkeep is a policy: `lazy` or `eager`, `rebuild` or `incremental`, and
`clustered` keeps the archetype's own rows in key order.

## Space: grids, boxes, the k nearest

```
index Cells of Atom grid (x, y, z) cell Melt.cut + Melt.skin wrap Melt.size { ... }

for o, b: Point nearest k to (a.x, a.y, a.z) in Cells where o != me { ... }
for o, b: Dot between (a.x - 300, a.y - 50) and (a.x + 100, a.y + 250) in Grid { ... }
```

Grids came first, for the Lennard-Jones melt in [post 06](06-science-models.md),
with `near`. Then integer grids, `between` (inclusive boxes, which also work in
wrapping worlds), and `nearest k` searched ring by ring.

The outside reference was PBBS's `octTree`, a zd-tree. The first numbers were
humbling. On one core, a million uniform 3D points, k=10: grid 1,742 ms against
octTree's 1,105. On a 100k Plummer cluster: **11,232 ms against 137**. Grids
are hopeless when density is uneven.

## Morton, and Barnes's treecode

```
index Bodies of Body morton (sx, sy, sz) leaf 1 wrap G.rsize clustered

for c in Bodies.cells {
  if c.leaf {
    for o, b: Body in c where o != me { ... }
  }
  ...
}
```

The Morton index is a linear quadtree or octree: a tree whose cells you can
walk yourself. Cells sum what you declare (`cells { m: sum e }`), and `open`
says "go into this cell's children". I picked the keyword `morton` over `tree`
and `octree`.

On kNN, one core, it took 0.75 to 1.18 times PBBS octTree's time, and was
faster on four of six sets. On all cores it was at first held back by building
the index serially. After making it in parallel (float `min=`/`max=` reduced
across threads, leaves boxed through windows), it took 1.04 to 1.25 times
octTree's time on all cores.

For the cell walk, the reference was Barnes's treecode 1.4 (from NEMO), on
its own Plummer model:

- `forces.ent` builds **identical cells and identical interaction counts**,
  and forces within 1e-13 of treecode's.
- A walk per body was 3.6 to 5× slower than treecode on one core. treecode
  walks once per *cube* of bodies and builds interaction lists.
- `groups.ent` does the same in plain ent, with no new syntax. The monopole
  version is **at parity on one core (1.02 to 1.05×)**, the quadrupole version
  is 1.5× slower, and on all cores both are **4 to 5.5× faster** than
  treecode.

There is one gap left: gcc vectorises treecode's inner loop with in-order
adds, and LLVM does that only with `-force-ordered-reductions`. We measured
turning it on everywhere: within 2% either way. So it isn't the default.

## The k-d tree

It's the same cells and the same queries as Morton. Only the making is new:
the median split on the widest side, three sorts, and stable splits by
running sums. A million uniform 3D points, k=10, one core:

| | ms |
|---|---|
| ent k-d tree | **922** |
| ent Morton | 973 |
| PBBS octTree | 1,098 |
| nanoflann 2.0.1 | 2,780 |

On all cores Morton still wins, because the k-d tree spends 35 ms making the
index, 32 of them in listing and three sorts. Two attempts to fix that didn't
pay.

## Relations: GAP

Relations had been stored as CSR since 1 October. The reference for them was
the GAP Benchmark Suite on a Kronecker graph with 2¹⁸ vertices:

- PageRank started at **151 ms against GAP's 41**. Reading a relation
  backwards went through a permutation, which meant scattered reads. Keeping
  the sources and targets in index order removed the indirection, for
  **45 ms**.
- BFS needed language features, not storage changes: `break`, `continue`,
  `for e, n: Node in Front` (a `for` over the entities a buffer names), and
  `set`, a bitmap over entities with `u in Front`. With them, BFS on one core
  takes **3.50 ms against GAP's 3.51**, and on all cores **0.68 against
  0.67**.

```
for (u)-[Link]->(e) {
  if u in Front {
    found = true
    break
  }
}
```

Edges now also take `by w first k` and `where v == x`, answered by an index
where one is declared. Top 3 by weight on the GAP graph: 2.3 ms indexed
against 52 without. Connecting and disconnecting mends the edge order in
place, PMA-style: 0.6 ms against 60 ms for re-sorting.

## Partition: the compiler picks

```
index HealthByStage of Health by stage partition
```

This one is for enums, bools and small integers. The one word covers two
storages, and **the compiler picks**: rows clustered by value, where nothing
changes the field, or a family of bitmap sets mended by reactive loops. The
first version, sorted runs, was dropped after measuring 21 ms against 0.64 a
frame.

Honest numbers: on the epidemic (a million people) it gains 10% on one core
and nothing with more threads. On the GPU it loses: 4.4 ms against 3.9 ms a day.
The tag `apart` it was meant to replace was much worse (160.8 ms a day on one
core). So partition is the right default to have, but not a miracle.

## Then the compiler writes the index for you

The last step was the one the whole design aimed at. Write the program the
plain way:

```
for o, b: Walker, c: Health where o != me &&
    (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) <= Square.reach * Square.reach {
```

and `ent build` says:

```
remark: visits every entity to find those within Square.reach of a place;
an index would find them: 'index WalkerNear of Walker, Health grid (x, y) cell Square.reach'
```

`ent build --infer-indexes` adds the indexes itself, and a trace from
`--tune` refines the static guesses. It covers partitions, hashes, sorted
indexes, grids (a pair loop with a squared distance becomes `near`) and
relation indexes.

We tested it both ways:

- The hand-indexed programs, with their index declarations deleted, get the
  same indexes back, with the same output and speed.
- A deliberately naive crowd program goes from **25.1 s to 0.65 s** on one
  core.

That's the point of putting indexes in the language: the compiler can see
that you need one before you do.

Next: [10 · Pixalorien: a hackathon game](10-pixalorien.md).
