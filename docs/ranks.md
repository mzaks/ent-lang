# Programs on many ranks

A program built with `tools/ent build --ranks N` (or run with `tools/ent
run --ranks N`) runs as several processes, the ranks, started by MPI
(`mpirun -n N program`): on the cores of one machine, or later on many
machines. It says what it says on one process, to the byte: where its
entities live changes how fast it runs, never what it computes. The design
and its phases are in the doc "ent-lang on many machines: design (D0)";
this is what is built (D1 and D2).

## Distributing entities

```
component Colony { number: i32, food: i64 }
component Bee { home: entity, energy: i32 }
distribute Colony by number   // ranges of the numbers, one for each rank
distribute Bee by home        // each bee where its colony is
```

- `distribute Component by field` is a top-level declaration, of a
  component declared here or in an imported module. An archetype that
  holds the component (not optionally) is distributed; one that holds two
  distributed components is an error.
- By a whole number: its values, from the least to the most, cut into as
  many equal ranges as there are ranks. By an entity: the entity lives on
  the rank of the one its field names, which must be distributed itself.
- Every rank runs the program's setup whole, as one process: `world`, and
  what `main` does before its first `loop`. So every rank spawns every
  entity (ids are the same on every rank) and makes every edge, and a
  `for` there may do anything it does on one process. Where `main` starts
  its first loop the entities are placed: each rank takes a stretch of
  each distributed archetype's rows, the same on every rank. The rows of
  one rank must come one after another in the field's order (spawned in
  that order); else the program stops. A program without a loop in
  `main` is never placed.
- An archetype without `distribute` that a `for` in the loop goes over
  together with a distributed one is distributed too, by its rows: as
  many even stretches as there are ranks. Any other is every rank's, and
  every rank goes through all of it, as on one process.

## Partitions: worlds larger than one machine

`tools/ent build --ranks N --partitions` builds for N partitions, one a
rank, run on N ranks. No rank ever holds the whole world:

- Each declared distributed archetype has rows of each partition's own:
  partition p the rows from p times C, C twice an even share of the
  capacity (rows a rank never uses cost it no memory). Every rank keeps how
  many each partition has, so an entity's id is the same on every rank.
- An entity goes to its partition where it is spawned: by a whole number,
  the value over a block (`ENT_BLOCK` where the program runs, else an even
  share of the capacity), the last partition taking the rest; by an
  entity, that entity's. Only the rank whose partition it is writes it. So
  the entities of a partition are those of a range of the values, in the
  order they were spawned: a block that fits the values spreads them
  evenly, one that does not leaves some partitions empty and others full
  (a partition that has no room stops the program, which says so).
- The setup is spread too: a `for` over distributed entities goes through
  each rank's own from the start. Code outside a `for` runs on every rank:
  each spawns every entity (writing its own), and keeps the edges it
  connects that have an end of its own.
- An archetype that a `for` goes over together with a distributed one,
  not declared so, is all partition 0's.

- A relation keeps, on each rank, the edges its entities are visited by:
  those whose source is its own, and, where the program also goes to a
  target's edges, those whose target is. They are sorted by key over the
  rank's own edges alone (a radix sort), and where each key's edges start
  is kept for the keys of the rank's own entities only; so for a tree.

- A field that a `for` looks up in other ranks' entities is gathered
  whole on every rank before the `for` (fast where it fits), or, with
  `ENT_GHOSTS=ask` where the program runs, asked for: a copy of the `for`
  that only looks up goes through the rank's rows first, asking for what
  it would read of other ranks' entities (again while a round asks for
  more), each rank answers from its own rows, and the `for` reads those
  answers. The `for`'s work that leads to its lookups is done twice; a
  rank holds of other ranks' entities only what it looked up. A `for`
  that chose whom to look up by what it wrote first would look up one it
  did not ask for: the program stops there.

- With `ENT_GHOSTS=ask`, a buffer that `for`s over distributed entities
  write, and only such `for`s (or fns only they call) read, is split by
  rows: a rank keeps the rows its entities wrote, the rows given alike
  outside a `for` (runs of rows all alike are kept as one, not written),
  and asks for other ranks' rows like a lookup. When such a `for` ends the
  ranks say whose rows they wrote instead of giving them. A row that was
  another rank's, written again by this rank's entity, must get every
  field (else the program stops). A `for` that reads a buffer by
  searching it (each read where the one before says) asks in as many
  rounds as the search has steps, its work done again in each: slow.

- `ENT_WORLD_DIR=dir` where any program runs (on many ranks or one) puts
  its world in a file in that directory (sparse, gone when it ends), which
  the system keeps in memory as far as it can and writes to the file past
  that: a world larger than the machine's memory runs, slowly. The
  epidemic with tracing at 1e6 people (1.15 GB) ran in 500 MB so, in
  203 s instead of 18 s, to the same bytes. (A directory on a disk: one
  in memory, as /tmp often is, is no help.)

- A process runs several partitions where it is started on fewer ranks
  than the program is built for: `mpirun -n 2 program` of a build for 8
  partitions runs 4 in each process, each a thread with a world of its
  own (in a file of its own, with `ENT_WORLD_DIR`). What the ranks do
  together the partitions of a process do through memory, and the
  processes through MPI. `ENT_RUNNING` says how many partitions of a
  process go through their rows at once (as many as there are cores; 1:
  one after another, which takes the least memory); one that waits for
  the others lets another run.

- The partitions of a process share memory, one copy for all of them, of
  what only its owner writes: the rows of distributed archetypes (so a
  field gathered for lookups is there for all at once), the places for
  what their `for`s send, connect and give to buffers, and buffers that no
  code outside a `for` both writes (or gives rows to, or hands to a proc)
  and reads. A `for` over distributed entities ends with all of them done
  before any goes on. `ENT_NOSHARE` gives each partition its own copy, to
  compare. The epidemic with tracing at 1e6 as 16 partitions in one
  process ends in 1.96 GB (4.06 GB without sharing), in 37.7 s; in a 2 GB
  limit, with its worlds in files on a disk and one partition at a time
  (`ENT_RUNNING=1`), in 172 s, where 16 processes took 1359 s.

Not yet: what the setup takes at its most (each partition builds its own
part with what every rank has whole) is not shared: the 16 partitions
above take 4.1 GB at the most.

## A `for` over distributed entities

From the placement on, each rank goes through its stretch of the rows. What the `for` does comes
to the same on any number of ranks:

- its own entities' fields, read and written;
- whole numbers added into uniques (`+=`, `min=`, `max=`), each rank's part
  added up over the ranks when the `for` ends (MPI's `Allreduce`);
- fractions added into uniques (or kept the least or the most): the ranks
  take turns in their order, each adding its rows to what the rank before
  it ended with, so the sum is one process's (such a `for`'s rows run one
  rank after another);
- running sums of whole numbers (`let first = Free += free`), each rank's
  rows from where the ranks before it end (`Exscan`); over several
  archetypes only of one number for every entity (`+= 1`);
- lookups of entities of any rank (`Colony(c.next).food`): before the
  `for` the fields it looks up are made whole on every rank, each rank's
  stretch given to the others (`Allgatherv`), so a lookup reads another
  rank's entity as it was when the `for` started, as on one process (a
  `for` does not look up a field it writes);
- sends to entities of any rank: whole numbers added (`+=`, `-=`) or kept
  the least or the most (`min=`, `max=`), which come to the same in any
  order. One to an entity another rank has is posted to that rank, one to
  an entity of an archetype that is not distributed to every rank; when
  the `for` ends the ranks swap them (`Alltoallv`) and each combines what
  it was given;
- lines it writes out (`put`), each rank's after the ranks' before it, in
  the order of the rows; a `for` that takes turns (`in turn by c.number`)
  writes each rank's in its order;
- edges gone along from its own entities (`for (p)-[w: Tie]->(o)`, and
  `in`), to entities of any rank, and the far end's fields read
  (`(o: Person)`, as lookups). An edge lives with its source's rank and,
  where its target is another rank's, with the target's too (with every
  rank where an end is an entity every rank has): when the entities are
  placed each rank keeps only those edges;
- edges connected (from the body or a loop of it, from and to entities of
  any rank), let go of (`t.disconnect()`) and their fields written: each
  sent to the ranks of the edge's ends and done there when the `for`
  ends, in the order of the ranks, so the copies of an edge stay alike
  and in the order they have on one process. An edge goes by the end its
  table is sorted by, the other end, and how many edges between the two
  come before it;
- going along a tree (`bottom up`, `top down`), where each child lives on
  its parent's rank (distributed by its parent, or both by one number):
  each rank goes through its own entities, and what each does to itself
  and its ancestors. Where a child lives on another rank than its parent
  the program stops when they are placed;
- rows given to buffers (`Log += {..}`): each rank's after those the
  ranks before it gave (`Exscan`), every rank given all of them;
- windows of buffers and rows of buffers at running-sum places, which each
  rank writes for its own entities. When the `for` ends every rank is
  given the rows the others wrote: each rank's from the least to the most
  row its entities wrote, field by field (`Allgatherv`). Where the
  stretches of two ranks overlap (a rank's entities wrote rows apart,
  with another's between them) the program stops.

Not yet, in a `for` of a system that runs in the loop, and refused when
the program is built: `=` into uniques, running sums of
fractions or over several archetypes of values that differ, a buffer
given its rows anew or fewer of them, spawns and destroys, moves to
another archetype, reactive `for`s, a `for` inside a `for`; in a `for`
that takes turns anything written to a unique or a buffer, or sent; in
one along a tree anything but what each entity does to itself and its
ancestors. Not yet, and the program stops when it happens: a lookup of an entity another rank has from a `for` over
entities every rank has or outside a `for`, a send to one of a fraction,
with `=` or to a field whose changes are events (with the rank, the row
and the line of the lowered program), and entities spawned into or gone
from a distributed archetype after it was placed.

## Devices

What a device does (an extern proc: the console, files, the clock) is done
by rank 0, and every rank is given what it gave back and what it left in
the buffers it was given to change, so the ranks go on alike: a file is
read once, the clock is rank 0's. In a `for` over distributed entities each
rank calls the device for its own rows; what they write to standard output
reaches rank 0, which writes it in their order.

An extern system (a system without a body, as `world`'s `save_world`)
works on the whole world, where each rank has only its own rows of a
distributed archetype: one that declares no reads or writes, or reads or
writes a distributed component, is refused when the program is built.

## Not yet

- Every rank runs the setup and so has, until the placement, every row:
  the most memory a rank takes is the whole world's. When it is placed each rank gives the memory of the other
  ranks' rows of its columns back to the system (`madvise`), and keeps
  its own; the row numbers stay. So it does with the places for what
  `for`s send, connect and give to buffers, with the copies sorting makes
  (of rows and of edges), and with the edges it no longer has. A field that a `for` looks up is had
  whole again from then on. Buffers, uniques, archetypes that are not
  distributed, trees, what is kept for each entity of a relation (where
  its edges start) and the table of entities every rank keeps whole.
- A `for` along a tree goes through every rank's rows, doing nothing for
  those of other ranks.
- `--gpu` is for one process. Systems' loops are fused on many ranks as
  on one, where all of them go over distributed entities or none.

## Running

- `tools/ent run program.ent --ranks 4` builds and runs on 4 ranks
  (`mpirun -n 4 --oversubscribe`). MPI (Open MPI or MPICH, `mpicc` and
  `mpirun` on the PATH) is needed where it is built and run.
- `ENT_RANKS_LOG=1` (passed on with `mpirun -x ENT_RANKS_LOG`) says each
  rank's stretch of each distributed archetype when it is placed, where a
  rank stopped, and as each rank ends how long it spent in each kind of
  call to the others (which is where it waited for them too), how many
  sends it posted to other ranks and was given, and its memory. With
  `ENT_RANKS_LOG=2` also, for each place the program calls the runtime
  from (by its offset in the program's file, which `nm` names), how long
  the rank worked before it and waited in it. With `ENT_RANKS_LOG=3`
  each rank also writes, as it ends, which pages of its world it has
  (`ent-ranks-pages.<rank>`: the offset of each MB and how many of its
  pages), and `ENT_LAYOUT_MAP=1` where the program is built prints where
  each array of the world starts: together they say what a rank's memory
  holds.
- The runtime is `devices/ranks.c`, the only code that calls MPI.
