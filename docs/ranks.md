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

## A `for` over distributed entities

From the placement on, each rank goes through its stretch of the rows. What the `for` does comes
to the same on any number of ranks:

- its own entities' fields, read and written;
- whole numbers added into uniques (`+=`, `min=`, `max=`), each rank's part
  added up over the ranks when the `for` ends (MPI's `Allreduce`);
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
- edges gone along from its own entities (`for (p)-[w: Tie]->(o)`), to
  entities of any rank: every rank has every edge (made in the setup);
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
the program is built: fractions and `=` into uniques, running sums of
fractions or over several archetypes of values that differ, a buffer
given its rows anew or fewer of them, spawns and destroys, moves to
another archetype, reactive `for`s, a `for` inside a `for`; in a `for`
that takes turns anything written to a unique or a buffer, or sent; in
one along a tree anything but what each entity does to itself and its
ancestors. Not yet, and the program stops when it happens: connects and
disconnects, a lookup of an entity another rank has from a `for` over
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
  its own; the row numbers stay. A field that a `for` looks up is had
  whole again from then on. Buffers, uniques, archetypes that are not
  distributed, relations and the table of entities every rank keeps
  whole.
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
  the rank worked before it and waited in it.
- The runtime is `devices/ranks.c`, the only code that calls MPI.
