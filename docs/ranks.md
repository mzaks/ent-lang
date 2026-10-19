# Programs on many ranks

A program built with `tools/ent build --ranks N` (or run with `tools/ent
run --ranks N`) runs as several processes, the ranks, started by MPI
(`mpirun -n N program`): on the cores of one machine, or later on many
machines. It says what it says on one process, to the byte: where its
entities live changes how fast it runs, never what it computes. The design
and its phases are in the doc "ent-lang on many machines: design (D0)";
this is what is built (D1).

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
- Every rank runs the program's setup and spawns every entity, so ids are
  the same on every rank. When a `for` first goes over a distributed
  archetype its entities are placed: each rank takes a stretch of the rows,
  the same on every rank. The rows of one rank must come one after another
  in the field's order (spawned in that order); else the program stops.
- An archetype without `distribute` is every rank's, and every rank goes
  through all of it, as on one process.

## A `for` over distributed entities

Each rank goes through its stretch of the rows. What the `for` does comes
to the same on any number of ranks:

- its own entities' fields, read and written;
- whole numbers added into uniques (`+=`, `min=`, `max=`), each rank's part
  added up over the ranks when the `for` ends (MPI's `Allreduce`);
- running sums of whole numbers (`let first = Free += free`), each rank's
  rows from where the ranks before it end (`Exscan`); over several
  archetypes only of one number for every entity (`+= 1`);
- lookups of and sends to entities of its own rank;
- lines it writes out (`put`), each rank's after the ranks' before it, in
  the order of the rows; a `for` that takes turns (`in turn by c.number`)
  writes each rank's in its order;
- windows of buffers and rows of buffers at running-sum places, which each
  rank writes for its own entities.

Not yet, and refused when the program is built: fractions and `=` into
uniques, running sums of fractions or over several archetypes of values that
differ, a `for` over distributed and copied entities at once, edges,
connects, rows given to buffers (`+=`), spawns and destroys, moves to
another archetype, trees, reactive `for`s, a `for` inside a `for`, and in a
`for` that takes turns anything written to a unique or sent. Not yet, and
the program stops when it happens: a lookup of or send to an entity
another rank has (with the rank, the row and the line of the lowered
program), and entities spawned into or gone from a distributed archetype
after it was placed.

## Devices

What a device does (an extern proc: the console, files, the clock) is done
by rank 0, and every rank is given what it gave back and what it left in
the buffers it was given to change, so the ranks go on alike: a file is
read once, the clock is rank 0's. In a `for` over distributed entities each
rank calls the device for its own rows; what they write to standard output
reaches rank 0, which writes it in their order.

## Not yet

- Every rank keeps every row of every archetype and buffer: the work is
  split, not the memory. A rank reads and writes only its own rows of a
  distributed archetype and of the buffers its entities write; the rows
  of other ranks it has are stale.
- A buffer row written by one rank and read by another is not given to
  it: the program must read only rows its own entities wrote (beecs
  does).
- `--gpu` is for one process. Systems' loops are fused on many ranks as
  on one, where all of them go over distributed entities or none.

## Running

- `tools/ent run program.ent --ranks 4` builds and runs on 4 ranks
  (`mpirun -n 4 --oversubscribe`). MPI (Open MPI or MPICH, `mpicc` and
  `mpirun` on the PATH) is needed where it is built and run.
- `ENT_RANKS_LOG=1` (passed on with `mpirun -x ENT_RANKS_LOG`) says each
  rank's stretch of each distributed archetype when it is placed, and
  where a rank stopped.
- The runtime is `devices/ranks.c`, the only code that calls MPI.
