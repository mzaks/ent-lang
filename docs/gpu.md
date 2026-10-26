# Systems on the GPU: design of G1

Status: steps 1 to 4 done (the marker and its checks; the lowering; the
runtime and `tools/ent --gpu`; measured), and after them the world kept
on the GPU by copies the compiler places instead of managed memory (see
Residency, and "After step 4" at the end for why), and one kernel for a
fused sequence. Of G2, lookups and whole-number sends are built; the rest
(fractions sent in the CPU's order, `math` rounding alike on both sides)
waits for a program that needs it (2026-10-17). G0 (`bench/gpu/`, results in
`bench/gpu/RESULTS.md`) showed that ent-opt's entity-parallel loops become
correct kernels through upstream MLIR passes alone, that a frame of 1e7
entities takes 3.75 ms with the world on the GPU against 14.5 ms on one
core, and that reading the world over PCIe each frame (29.5 ms) is slower
than one core. So the columns have to stay on the GPU between frames, and
G1 is about the language saying which systems run there, the compiler
checking that they can, and the compiler, not the program, deciding where
each column is.

Goals of G1:

- A system can be marked to run on the GPU; the compiler says, with a
  reason, why a marked system cannot.
- A program gives the same results with and without the GPU, bit for bit
  (the determinism rule of `sync-points.md`).
- Nothing in the program moves data: residency follows from what each run
  reads and writes, which the compiler already knows.
- `tools/ent build --gpu` builds such a program; without `--gpu`, or on a
  machine without a GPU, the same program runs on the CPU.

Not in G1: anything that combines across entities (applies, accumulates,
rows given to a buffer), lookups, edges, trees, reactive queries, extern
fns including `math`. Each is an error for now, with the stage that
brings it (see "What a GPU system may do").

## The marker: `gpu system`

```
gpu system integrate(dt: f32) {
  for p: mut Position, v: Velocity {
    p.x += v.dx * dt
    p.y += v.dy * dt
  }
}
```

A prefix, like `extern system` and `transient unique`. It is a contract,
like `reads` and `writes`: the compiler checks it whether or not the
program is built for a GPU, so a program does not stop being able to run
there without being told. It changes nothing in what the system means:
placement is one more execution freedom, like stages and parallel loops.

Every `for` of a `gpu system` runs on the GPU, each as one kernel (or,
with fusion, one kernel for consecutive `gpu` systems, see Lowering). The
system's own code outside its `for`s (reading and writing uniques, a
counted loop around a `for`) runs on the CPU, as it does now.

Why on the system and not on the `for` or the run:

- On the `for`: a system whose `for`s run in two places has to wait for
  the GPU between them anyway; one marker per system keeps that visible.
  A system that needs both is two systems.
- On the run in a schedule (`integrate(dt) on gpu`): placement would then
  differ between schedules running the same system, and so would the
  columns' residency. Possible later; not needed to start.
- A compiler option alone (`--gpu=integrate`): gives no place for the
  contract to live, and G3 (placement by profile) is the automatic one.

In the dialect: a unit attribute `gpu` on `ent.system`
(`ent.system @integrate(%dt: f32) gpu { ... }`), checked by the
system's verifier.

## What a GPU system may do

In a `for` of a `gpu system`, G1 accepts what touches only the visited
entity and values every entity reads alike:

- reading and writing its own components (`ent.get`, `ent.set`), its id,
  `has`, filters, `where`, `optional` bindings;
- `add` and `remove` of a component kept in place (not `apart`): they
  write the presence byte of the entity's own row;
- reading uniques and consts: passed to the kernel as arguments, as the
  CPU loop already reads them once before it;
- reading a buffer (`len`, `Name[i].field`), also through a fn it is
  given: a buffer is treated as a column (see Residency);
- looking up another entity's fields (`if let x = C(other).f`), which
  the query does not write (G2: the entity table, the slots handed out
  and their generations and locations, is kept on the GPU as pieces);
- sending whole numbers to other entities and accumulating them into
  uniques with `+=`, `-=`, `min=`, `max=` (`Health(foe).hp -= 3`,
  `Hits += 1`): the same in any order, so combined where they are sent,
  by atomic operations (G2). A unique's cell is the GPU's copy's: the CPU
  sets it to where the rule starts before the loop, and combines what the
  loop made of it into the unique after (`ent_gpu_cell_set`,
  `ent_gpu_cell_get`, one wait for the GPU);
- accumulating fractions into uniques with `min=` and `max=` (`Box.lo
  min= d.x`, 2026-10-25): the same in any order too. The cell holds the
  fraction's bits in their order (the bits but the sign turned over where
  the sign is set: as whole numbers with a sign they order as the
  fractions do, -0.0 below 0.0), combined by whole numbers' atomics,
  starting at the infinity on the other side; the CPU turns them back;
- going along a relation's edges from either end (`for (p)-[w: R]->(o:
  mut C)`, `for (o)-[w: R]->(p)`): the edges' fields, written too
  (`mut w`), the other end's components, and what is sent to it (G2: the
  sorted table, the offsets of each entity's edges and the index by
  target are pieces, copied as many edges as there are). Not a tree's;
- sending and accumulating whole numbers from loops of the body (`for k
  in a..b`, `while`): combined atomically where they are sent, as from
  the body itself (on the CPU they go on lists, combined in the order
  of the rows and turns);
- windows of buffers (`w: mut Cohorts[c.cohorts, Colonies.rows]`,
  docs/syntax.md), read and written: the rows are taken by an atomic
  exchange each (the run and the entity's id), the CPU's run counter
  given to the kernel; overlapping windows, and rows outside one, are
  said in the buffer's flag the CPU reads when the `for` ends; and
  handed to fns and procs of the program (`cohorts: mut Cohorts[]`, an
  i64 there, start and count), a proc too where it changes only rows it
  is given;
- running sums (`let first = Laying.stubs += s.meets`, docs/syntax.md):
  in two passes, with a scan of the values between them (below);
- calling `fn`s of the program: compiled for the GPU like the body;
- a tree kept by links (not `sorted`): a ref to the parent
  (`-[LivesIn]->(h: Household)`), read through the edge in the child's
  slot, which the GPU keeps as a piece; and `top down` or `bottom up`
  along a tree of one level (no archetype holds the components of both
  ends, so nobody is a parent and a child), which is a `for` over the
  children whose whole numbers combined into the parent (`+=`, `min=`,
  `max=`) are combined atomically, as sends are (`flattenGpuCascades`);
- reading tables (`table`), but for their texts: each field a constant
  array the kernels read, copied into the GPU's module with the code
  that reads it;
- arithmetic on every number type. `f64` is allowed; on consumer cards
  it runs at 1/32 to 1/64 of `f32`, which `--explain` says.

Errors, each naming the construct and what it waits for:

| In a `for` of a `gpu system` | Why not yet | When |
|---|---|---|
| sending a fraction (`f32`, `f64`), accumulating one but by `min=`, `max=`, or sending with `=` | combined at the commit point in the order of the rows, which the GPU does not keep: needs the sends sorted by target and combined in that order on the GPU (keep such a system on the CPU) | later |
| sending to a field the query also reads or writes | what is sent lands when the query ends; on the GPU it lands as it is sent | never |
| `extern fn` | the C runs on the CPU; the GPU's own functions round differently from the machine's library, which breaks the same-results rule (`math` is written in ent and runs on both, see below) | G2 |
| a `for` over other entities inside the `for` | fine on a GPU (it is its best case), but needs the `old` copy and the inner loop's order kept | G2 |
| further up a tree than the parent (`*`, `hops`, siblings, paths), a tree's edges (`for` over them), a `sorted` tree | the links and the lists in order are the CPU's | later |
| `top down`, `bottom up` where a parent may be a child too, or with a fraction combined | needs the depths one after another (a list of each) | later |
| `connect`, `disconnect` | the relation is sorted again on the CPU | later |
| `on changed ...` (reactive) | event logs and stamps | later |
| `spawn`, `destroy`, `add`/`remove` of an `apart` component | change which rows an archetype holds; the compaction runs on the CPU | later |
| `by` whose body does more than read and give each entity's rows to buffers (each `+=` in the body itself); `in turn by` | one entity after another: no parallel form | never |
| a text (`text[N]`, `text`), a table's too | not on the GPU yet | later |
| a fn given the entities of a component | it reads other entities through a function of the CPU's, given the world | later |
| `extern proc` | the outside world | never |
| writing or sending to a component a reactive query reacts to, or to the field a tree is ordered by | its events (stamps), and the tree's order, are kept on the CPU | later |

An extern system cannot be `gpu`. The code of a `gpu system` around its
`for`s reads and writes uniques and computes; it may also empty a
buffer, keep its first rows, give it rows alike and ask how many it has
(`Cells = []`, `Cells = Cells[..n]`, `Cells += [{ .. }; n]`, `len(Cells)`,
2026-10-25): the count is the CPU's, set there and given to the GPU's
copy at once (`ent_gpu_cell_set`) for the loops after; the rows given
are filled by a loop for the GPU. So a `while` around `for`s can make a
buffer level by level (`Inputs/gpu_buffers.ent`). What else it would do
to entities or buffers (spawn, `Marks += { .. }`, a row read, a lookup)
goes in a system of its own, so that every run touches the world's
columns from one side.

## Lowering

ent-opt gets an option `gpu=1` on `--ent-lower-to-loops`. Without it, a
`gpu system` is lowered as any other (parallel on the CPU with
`parallel-entities`). With it:

1. Every query of a `gpu` system becomes an `scf.parallel` over its rows
   whatever its archetype's capacity and count: no threshold and no
   sequential twin, so a column never goes back to the CPU because there
   were few entities this frame.
2. The loop is tiled by 256 and given the `#gpu.loop_dim_map` mapping
   itself, so that `convert-parallel-loops-to-gpu` takes exactly these
   loops and `convert-scf-to-openmp` the CPU's, in one program. (G0 ran
   `gpu-map-parallel-loops`, which maps every parallel loop.)
3. Fusion fuses consecutive `gpu` systems with each other, never a `gpu`
   one with a CPU one (`fuse-systems` already stops at many kinds of
   system; placement is one more).
4. The pipeline after ent-opt is G0's: kernel outlining, then
   `gpu-lower-to-nvvm-pipeline` for the chip, PTX embedded in the program
   and compiled by the driver when the program starts.

Floating point: the kernels must round as the CPU does. LLVM emits no
`contract` flags for ent code, so NVPTX keeps `mul.rn` and `add.rn`
apart (G0's checksums agree). The GPU test mode (below) keeps it so.

## Residency

The GPU keeps a copy of the world: the CPU's layout, the same offsets, at
another address (`ent_gpu_arena`, one allocation of the world's size).
The GPU's loops address that copy; counts and uniques stay the CPU's,
which a kernel is given as its loop's bounds and arguments.

A **piece** is what both keep of the world: a column of an archetype that
a `gpu` system touches (a field, a presence byte, the ids), a buffer's
field or its count. Of each piece the runtime keeps which bytes the CPU
has newer and which the GPU does (one range each). In a schedule the
compiler places, from each run's access (the reads and writes
`--ent-schedule` forms stages by):

- before a run, for the pieces it touches, `ent_gpu_to_device` (a GPU
  run) or `ent_gpu_to_host` (a CPU run): copied where the other side has
  them newer, as many bytes as are in use (rows times the element);
- after it, `ent_gpu_wrote` for the pieces it writes: newer where it ran.
  A CPU run that only gives an archetype rows (spawns, and does nothing
  else to its rows) marks only the new rows, so the GPU gets those and
  not the column;
- runs of one kind one after another do that once, around all of them,
  so that fusion still sees them together; a stage of both kinds does
  both; a run under a condition copies only where the condition holds;
- an extern system that declares nothing touches and writes everything;
  a proc never gets the world, so a system that calls one is not opaque
  here as it is to stages;
- when the schedule ends the CPU waits for the GPU (`ent_gpu_wait`), so
  that a frame is done when it returns.

A piece the CPU only reads is copied to it once and not back; a piece the
GPU alone goes through is never copied after the first frame. A C host
that reads or writes the world between schedule calls calls
`ent_gpu_world_to_host()` first (declared by the header with `ENT_GPU`):
everything the GPU has newer comes back, and from then on all of it is
the CPU's, copied to the GPU again where a GPU run needs it. (So H1
holds with that call.)

What the GPU has newer comes back as the words that changed, where
there is a lot of it (256 KB or more of a piece): the runtime keeps a
**shadow** of such a piece in the GPU's memory, what the CPU holds of
it (but for bytes the CPU wrote and has not given the GPU yet: giving
them makes the shadow theirs too). A kernel of the runtime's
(`ent_gpu_diff`) compares the range with the shadow word by word, makes
each word that differs the shadow's and packs it with its index; the
CPU gets the pairs in one copy and puts the words in place. One in
eight words or more, and the range comes whole after all (the kernel has
made the shadow the GPU's anyway). So a `for` that changes a few
entities of a million, wherever they are, sends back those: rows of a
block would not do, since the few are spread over all the blocks.
`ENT_GPU_WHOLE` turns it off; `ENT_GPU_REPORT`, `ENT_GPU_CHECK` and
`ENT_GPU_LOG` say what came back, check each packed copy against the
whole range, and log the first copies.

Where the CPU waits: only where it copies from the GPU, and when a
schedule ends. A copy to the GPU is from the CPU's memory as it is
(pageable): the driver has taken the bytes when the call returns, so the
CPU may write them right after.

Before this the world was in managed memory and the driver moved pages
to whichever side touched them; that was right only while the CPU kept
off what the GPU goes through (see "After step 4").

## Runtime: `devices/gpu.c`

The functions MLIR's lowering calls (`mgpuModuleLoadJIT`,
`mgpuModuleGetFunction`, `mgpuLaunchKernel`, `mgpuStream*`, unload), and
ent's own: `ent_gpu_arena`, `ent_gpu_to_device`, `ent_gpu_to_host`,
`ent_gpu_wrote`, `ent_gpu_wait`, `ent_gpu_world_to_host`. One stream:
`mgpuStreamCreate` gives it to every launch, and the synchronize and
destroy the lowering calls after a launch do nothing, since waiting is the
compiler's (above). One world. MLIR's ROCm wrappers have the same `mgpu*`
names, so another vendor is another file.

It is a device in the sense of `devices/`: C, linked by `tools/ent` with
`-lcuda` in a `--gpu` build, not imported by programs.

For finding out how a program goes there, environment variables:
`ENT_GPU_TIMES` (at the end, how long each kernel ran on the GPU, by name,
and each kind of copy, from a pair of events around it on the stream;
and how long the CPU waited for the stream; `ENT_GPU_TIMES_EACH=name`
says each launch of the kernels whose name has that in it),
`ENT_GPU_REPORT` (how much
came back whole and packed), `ENT_GPU_LOG` (the copies, a line each),
`ENT_GPU_CHECK` (each packed copy against the whole range),
`ENT_GPU_PROTECT` (each kernel with the CPU's world out of reach) and
`ENT_GPU_WHOLE` (no shadows).

## Tools and tests

- `tools/ent build --gpu`: ent-opt with `gpu=1`, the GPU passes, the chip
  from the machine (`nvidia-smi --query-gpu=compute_cap`, or
  `ENT_GPU_CHIP`), linked by the machine's compiler (the toolchain's own
  sysroot clashes with the system's libcuda, G0). The flag goes into the
  build cache's key.
- The integration tests, which build every example sequentially, fused,
  with parallel stages and parallel loops and compare outputs, get a GPU
  mode where the machine has one, skipped where not.
- Lit tests for every error of the table, and for the marker in the
  dialect's syntax.
- `examples/integrate.ent` with its four entity-local systems marked
  `gpu`.

## Steps

1. **Marker and checks.** `gpu system` in the parser, the attribute, the
   verifier's errors, `syntax.md` and the execution freedom in
   `sync-points.md`. No GPU needed; can be built on any machine.
   Built: `SystemOp::verifySymbolUses` checks it (`lib/Ent/EntOps.cpp`);
   tests in `test/Import/gpu.ent` and `gpu-errors.ent`; the four systems
   of `examples/integrate.ent` are `gpu`. A move into an archetype that
   does not hold the component optionally (rather than `apart`) is only
   known once the archetypes are inferred, so step 2 catches it.
2. **Lowering.** `gpu=1`: unthresholded mapped loops, fusion by
   placement, waits from the conflict relation, hints, the 64 KB layout.
   Checked as IR by lit tests. Built but for the hints, which wait for
   step 4 to show what they are worth: `test/Transforms/lower-gpu.mlir`
   and `lower-gpu-invalid.mlir`. Through G0's runtime,
   `examples/integrate.ent` gives the CPU's checksums at 1e4, 1e6 and
   1e7 entities, 3.75 ms a frame at 1e7. The loops are tiled and mapped
   at the end of `--ent-lower-to-loops` (`mapGpuLoops`), and its waits
   are calls of `ent_gpu_wait`.
3. **Runtime and tools.** `devices/gpu.c`, `tools/ent --gpu`, the GPU test
   mode; `bench/gpu/run.py` goes through `tools/ent` instead of its own
   pipeline. Built but for the last (step 4 does it, on the C host the
   bench needs, which `tools/ent` does not build):
   - `devices/gpu.c`: one stream for every launch, the launch's own
     synchronize a no-op, `ent_gpu_wait` waits only where something was
     launched since. Memory of a megabyte or more is managed (the world;
     `ENT_GPU_MEM=host` puts it in pinned host memory to compare); a
     program's own memory comes from it through MLIR's generic
     allocation functions (`finalize-memref-to-llvm` with
     `use-generic-functions`), a C host's world through the header with
     `ENT_GPU` defined. (All of that went with managed memory, after step
     4: see Residency.)
   - `ent-opt` registers every translation to LLVM IR, so it makes the
     kernels into PTX itself (`gpu-module-to-binary`); `tools/ent --gpu`
     runs the passes of `gpu_passes` around the CPU's, the columns
     already lowered to addresses (`--ent-lower-views`) so that a kernel
     is given those. The chip is nvidia-smi's, or `ENT_GPU_CHIP`; PTX 8.0
     (`ENT_GPU_FEATURES`).
   - `test/Integration/gpu.test` (lit feature `gpu`: nvidia-smi and
     `cuda.h`): `Inputs/gpu_mix.ent`, CPU and GPU systems that spawn,
     destroy, read and write each other's columns, prints the same built
     plain, `--gpu` and `--gpu --parallel`; integrate through its C host
     gives integrate.test's output.
4. **Measure.** `integrate` against G0's table (the floor with one stream
   and no wait per launch, with and without the 64 KB layout, with and
   without hints), and a program where the CPU spawns into an archetype a
   GPU system visits every frame, to see what a spawn costs. Into
   `bench/gpu/RESULTS.md`, native builds.

## Open questions

- Where the waits go when `main` loops: if a CPU system in the loop reads
  a column a GPU system writes, every frame waits; worth an `--explain`
  remark so the program can be split differently.
- `math` on the GPU (G2): settled, ent-lang implementations in
  `math.ent` used on both sides (below, "`math` alike on both sides").
  All of it now, `asin` and `acos` last.
- Whether a `gpu system` built without `--gpu` should say so (a remark),
  or stay silent as `--parallel`'s absence does.

## After step 4

Measured 2026-10-17 (`bench/gpu/RESULTS.md`):

- Where the CPU stays off what the GPU goes through, G1 is what G0
  promised: integrate at 1e7 in 3.7 ms (one core 12.4, OpenMP 10.5), the
  floor 11 us a frame, of which 10 are the driver's three launches and
  one synchronize.
- The 64 KB layout measured nothing; it is taken out again.
- Where the CPU spawns into or reads a column the GPU writes every
  frame, managed memory is slower than the CPU alone (1.2 to 15.5 ms a
  frame against 0.27 to 0.96), and neither the driver's advice nor
  prefetches fix it; explicit copies are about 3x better than the best
  prefetches.

So the hints are not worth building. Instead, in order:

1. **Copies the compiler places** (instead of managed memory), built
   (see Residency): integrate's floor 8 us a frame (11 with managed
   memory), 3.65 ms at 1e7; the CPU spawning a thousand a frame into a
   million the GPU moves 0.04 ms a frame (managed 1.2, the CPU alone
   0.29), reading a column the GPU writes 0.98 ms (managed 2.8, the CPU
   alone 0.78, its adding up included). The CPU's world pinned made the
   last 0.99 instead of 1.27 in one run, at the price of locking all of
   it in memory: not done. As planned: the
   columns `gpu` systems touch get a second home on the GPU, and the
   waits of step 2 become "copy what the CPU needs, of the rows in use,
   to the CPU" and "copy what the CPU wrote back before the next GPU
   run". The compiler knows both from the runs' access, as it knows the
   waits; a column the CPU only reads is not copied back, and rows the
   CPU spawned are copied as rows. The C host and extern systems then
   need a copy at the schedule's end (or a call to ask for one), which
   the header can do.
2. **Fewer launches a frame**: the loops of one fused sequence over
   several archetypes as one kernel (blocks for each archetype's rows),
   or a CUDA graph per schedule; 2.5 us a launch is most of the floor.
   Built, the first way (`fuseRuns`): one loop over the rows of all the
   archetypes one after another, a row finding its archetype by where it
   is among them. Integrate's frame is one kernel instead of three, its
   floor 6.3 us instead of 8 (one launch and one synchronize alone take
   about 5); at 1e6 and 1e7 the same as before. A `gpu` system that is
   not fused (a buffer read, a condition) still has a kernel for each
   archetype its query visits.
3. Then G2 as planned (ordered reductions, lookups, `math`).


## Audit: the epidemic (2026-10-17)

What keeps `examples/epidemic` off the GPU, found by marking each of its
38 systems `gpu` alone and building it (the verifier, then
`--ent-lower-to-loops=gpu=1`), against what a day costs on the CPU
(`bench/epidemic/RESULTS.md`, a million people on all cores):

| system | of a day | what keeps it off the GPU |
|---|---|---|
| `workplace` | 19.2 ms, about 60% | edge loops (`WorksWith`, from both ends); fractions sent (`o.left -= ...`) |
| `chance` | 6.1 ms, about 20% | fractions sent to `Hazard.left`; sent from a loop of the body (`for stub in ...`) |
| `pair` | 2.8 ms | a buffer filled outside its `for` (`lay` gives `Order` and `Index` rows) |
| `household` | 1.2 ms | `bottom up` along a tree (a sum per household) |
| `symptoms`, `infect` | 1.5 ms | `math` (`log64`, and `sqrt64` through the gamma draws) |
| `trace`, `told` (with tracing: 19.4 and 7.0 ms) | most of a day with tracing | edge loops (`Housemate`, `ToldBy`), `connect` and `disconnect` in them, `math` (`log64` in `stay`) |

What is on the GPU already (`infectiousness`, `tally`, `counts`,
`remember`, `apps`) is 3 to 5% of a day, and `infectiousness` writes what
`workplace` and `chance` read on the CPU: marking it would cost a copy a
day as big as the system. The systems of the set-up (tables, buffers) run
once.

So, by how much of a day each would bring to the GPU:

1. **Edge loops on the GPU**: `workplace`, `trace`, `told`, `household`.
   The edge tables as pieces; loops that read the edges and send to their
   other ends, which is what a GPU does well. `connect` and `disconnect`
   stay the CPU's.
2. **Fractions sent**: `workplace`, `chance`, `household`. Done in the
   model instead (below): the hazard in 2^-32ths, so what is sent is a
   whole number.
3. **Sends from a loop of the body**, for whole numbers sent atomically:
   `chance`. Only the CPU's lists keep it off.
4. **`math` alike on both sides**: the tracing's systems and the drawing
   of who falls ill how.

With 1 to 3, some 90% of a baseline day could be on the GPU.

**Edge loops, built (2026-10-17).** `workplace` now builds as a `gpu
system`: with it and `infectiousness` on the GPU (`bench/epidemic/run.py
1000000 --gpu workplace,infectiousness`, on the slow cores while another
experiment had the fast ones) a day of a million people takes 92.6 ms on
one core and 35.7 on four threads, against 180.9 and 67.8 on the CPU
alone; `workplace` 4.8 ms where it took 89.2 and 37.9. The same 857,886
are infected in a hundred days. Two things it took besides the edges'
pieces: kernels named without the `.` of a module's systems
(`--ent-gpu-kernel-names`), and sends by the same rule to one field from
two edge loops counting as unseen (in whole numbers they add up the same;
so does a lookup that only asks whether the other end has the
component). They are
also what a world in pieces over several GPUs or machines needs (edges
between the pieces, sends from many places combined in one order).

**Sends from loops of the body, built (2026-10-17).** A whole number
sent or accumulated in a loop of a `gpu system`'s body is combined
atomically where it is sent, as one from the body is; the CPU's lists
(`ent.looped`) are left out for it. `chance` builds as a `gpu system`
with no change to it (its `met_by` reads `Order` and `Index`, buffers the
GPU keeps). With `workplace`, `infectiousness` and `chance` on the GPU
(`--gpu workplace,infectiousness,chance`, the slow cores again) a day of
a million people takes 22.4 ms on one core and 15.7 on four threads,
against 179.2 and 67.5 on the CPU alone; the same 857,886 infected.
`workplace` is down to 1.75 ms as well: the hazard it sends to stays on
the GPU for `chance`, where before it went back to the CPU between them.
What is left of a day is mostly `chance` (5 ms, of which most is the CPU
waiting for the GPU and taking the hazard back for `infect`), `pair`,
`household`, `infect`, `progress`, `symptoms`: each 2 to 3.5 ms.

**Running sums (2026-10-17).** What keeps `pair` on one thread, on the
CPU too, is that where a person's stubs start is a running sum of the
stubs of those before them. The language has it now: `let first =
Laying.stubs += meets` in a `for` gives what the entities before this
one sent, in the `for`'s order, and the unique all of it. One entity
after another (a build for one core) the field is summed into as the
loop goes. Where the loop may be parallel or the GPU's, `lowerRunning`
makes two passes of it: a copy of the query that keeps only what the
values summed are made of (and the `if`s and loops around them) leaves
each row's value in a scratch array (a place for each of the query's
rows, in their order; on the GPU in its memory, `ent_gpu_scratch`); a
scan makes each place what came before it; then the query runs, each
running sum read from its row's place. The scan is parallel loops like
any other, so the GPU's path takes them as they are: chunks of 64
combined, one by each turn, level after level until one chunk is left,
which one turn scans, and back down each chunk scanned from where its
sum says it starts. Whole numbers come to the same in any grouping, so
every build gives what one after another does (`test/Integration/
running.test`, `gpu.test`). A query with one is not fused with others:
its first pass would read what an earlier body had not written yet.

**Rows of buffers written from a `for` (2026-10-17).** The other thing
`pair` needed: `Order[Pairing.order + row].who = e` in a `for`, the row
from a running sum, and the rows of `Index` for the stubs at a multiple
of 64. A row is written where it stands, and each entity writes rows of
its own, which the compiler sees in the row (`ownRowOf`): a place the
same for every entity, plus what a running sum gave the entity, plus an
offset inside its interval (none for a sum that adds 1, `k` of `for k
in 0..` the value added). Intervals do not overlap where no value added
is below nought. What is not seen when compiling is asked where the
entity runs, by a comparison or two (`ent.rows_check`): where it does
not hold the write is not made and the program stops after the `for`,
the same on every build, on the GPU too, with no atomics. (A first
version kept who wrote each row beside the buffer, 8 bytes a row, and
looked at it on every write: on many threads of the CPU a locked
exchange cost more than the loop, and without one a clash could go
unseen.) `lay` writes `Index` from a second running sum, how many of an
entity's stubs fall on a multiple of 64; it is made of the first (the
sum of those before telescopes to where the entity's first such stub's
row is), so the lowering makes a pass for each: sums whose values are
made of sums not worked out yet wait for the next. After its scan, what
a whole-number sum of the body itself (not an `if` of it) adds is read
back, its row's place less the next (`ent.running_added`; the scan
leaves the whole sum after the rows): the passes after it, and the
query, do not work out `meets` again, which made the second pass cost
nearly what the first does (`lay` on four threads 3.27 ms with it).

**`math` alike on both sides (2026-10-17).** `exp`, `log`, `pow`, `sin`,
`cos`, `floor`, `ceil`, `round` and `trunc` (f64, and f32 by way of them)
are written in `devices/math.ent` from `+ - * /`, the built-in `sqrt` and
the bits of floats (`bits_of`, `float_of`), all of which IEEE 754 rounds
exactly: fdlibm's algorithms as FreeBSD's msun and Mojo's standard
library have them (Mojo's comments and constants, checked against π and
ln 2 worked out to 450 digits), `pow` from a logarithm in two floats
(their notices and licenses in THIRD_PARTY_NOTICES).
Against glibc, on 120,000 inputs each over the whole range (angles to
2^1023 by the bits of 2/π), every result is within an ulp. On the GPU a
multiply and an add are never made one fma (the PTX has `mul.rn` and
`add.rn`, which `ptxas` keeps apart), and `math.sqrt` in a kernel becomes
`llvm.nvvm.sqrt.rn.f`/`.d` (`ent-gpu-kernel-names`): libdevice's f32 root,
which the lowering to NVVM calls, is an approximate one unless the module
asks otherwise. `test/Integration/Inputs/gpu_math.ent` gives the same
bits with and without `--gpu`. What the functions make not a number is
`0x7ff8000000000000` on both sides; that of an operation (`0.0 / 0.0`)
is the machine's, as IEEE 754 leaves it.

`tan`, `atan`, `atan2`, `log2` and `fmod` followed (2026-10-17), from
fdlibm's k_tan.c, s_atan.c, e_atan2.c and e_log2.c, `fmod` by long
division of the fractions a bit at a time (exact, as musl's): within an
ulp of glibc on 120,000 inputs each, `fmod` and `log2` of every power of
2 exact, glibc's results to the bit at zeros (their signs too),
infinities and not a number. They found that `!=` of floats was the
ordered comparison, false where one side is not a number: `x != x`,
which the module asks of every argument, never held, and `sin` of an
infinity was a number. It is now IEEE 754's (`cmpf une`).

How fast they are, ns a call over a million entities on one core
(a Zen 5c core, CPU 6, `-march=native`), against glibc's:

| f64 | ent | glibc | f32 | ent | glibc |
|---|---|---|---|---|---|
| `tan64` | 21.8 | 20.9 | `tan` | 6.6 | 5.7 |
| `atan64` | 9.3 | 13.3 | `atan` | 2.0 | 6.7 |
| `atan264` | 21.6 | 24.7 | `atan2` | 7.5 | 9.6 |
| `sin64` | 15.8 | 19.4 | `sin` | 11.9 | 10.8 |
| `sin64` of 1e10 | 17.5 | 107.5 | `sin` of 1e10 | 18.9 | 11.7 |
| `exp64` | 1.8 | 3.6 | `exp` | 2.3 | 2.2 |
| `log64` | 3.3 | 3.2 | `log` | 1.4 | 2.8 |
| `log264` | 3.7 | 3.5 | `log2` | 1.4 | 2.8 |
| `pow64` | 10.9 | 9.9 | `pow` | 6.8 | 4.8 |
| `asin64` | 7.9 | 17.9 | `asin` | 1.9 | 6.4 |
| `acos64` | 9.3 | 17.5 | `acos` | 1.9 | 6.4 |
| `fmod64` | 8.7 | 6.7 | | | |
| `fmod64` of 1e300 | 255 | 626 | | | |

`fmod` divides the fractions 11 bits at a time with `%` (a bit at a
time it was 20.3 and 1208). `fma(a, b, c)` is built in (`math.fma`,
made LLVM's own by `ent-gpu-kernel-names`, `fma.rn` in the PTX, where
the lowering to NVVM would call libdevice's, which a build without
`CUDA_PATH` does not link): IEEE 754 rounds it once, so it is the same
on both sides, and `two_prod64` is one (`pow64` 53.0 before). What is
left of `pow`, `log` and `log2` is their chain of operations, three
divisions in `pow`'s, and none of them vectorized: glibc's take theirs
from tables, with no division.

The f32 ones have polynomials of their own, worked out in f64 and
rounded once (FreeBSD's k_sinf.c, k_cosf.c, e_rem_pio2f.c, s_atanf.c,
k_logf.h; `tan` the quotient of `sin` and `cos`, as k_tanf.c's was an
ulp off one time in eight): on 100,000 inputs each, each is within an
ulp of the rounded true value and rounds it right 99.94 % of the time
or more, `exp` and `pow` always (glibc's `sinf`, `cosf`, `logf` 98.8 to
99.1 %). Before they were the f64 ones rounded: `sin` 20.8, `tan` 24.2,
`atan2` 23.6. A quarter turn, and which of `atan`'s ranges, are chosen
without a branch (which of them it is is anyone's guess, and a guess
gone wrong costs more than working out both): `sin64` was 19.2 and
`atan2` (f32) 21.3, while `atan64` of numbers to 100 (nearly all in its
last range, which a branch guesses right) was 6.3. `pow` (f32) is e to
the power of y * log(x), both to the last bits of an f64.

`pow64` and `pow` are Arm's (optimized-routines' pow.c and powf.c, as
glibc has them), from tables (`table`, read by the kernels as the CPU
reads them): log(x) from the row of the c near x's fraction (128 rows,
1/c and log(c) in two floats) and a polynomial of z/c - 1, exact by
`fma`; e^x from 2^(k/128) (a table of their bits) and a polynomial of
what is left. No division. `pow64` was 45.5 ns, within 14 ulp near the
ends of the range; now 10.9 (12.4 until x above 0 and y of no end went
there by one test of their exponents, as in Arm's), within 0.52
(agreeing with glibc's to the bit on 99.95 % of 240,000 inputs and
within an ulp on the rest, and on every one of 1,296 pairs of ends).
`pow` (f32) has its own (16 rows of log2, 32 of exp2, in f64; within
0.82 ulp): glibc's `powf` to the bit; it was 39.5 ns as `exp64(y *
log64(x))`, correctly rounded, and 14.5 as `pow64` rounded.

`log64` and `log264` are Arm's too (log.c and log2.c): near 1 a
polynomial of x - 1 (there log(x) is too small for the table's c to
leave its last bits), else as `pow`'s logarithm, with 128 rows (64 for
`log2`) of 1/c and log(c) and r = z/c - 1 by `fma`. They were fdlibm's,
9.0 and 11.6 ns, each with a division; now 3.3 and 3.7, and glibc's to
the bit on 99.996 % and 99.94 % of 242,000 inputs (within an ulp on the
rest, exact at every power of 2).

`asin` and `acos` are fdlibm's (e_asin.c, e_acos.c), and `math` takes
nothing of C any more (devices/math.c is gone). Within an ulp, glibc's
(correctly rounded) to the bit on 95.5 and 96.8 % of 180,000 inputs.
The f32 ones round rightly on all 180,000: e_asinf.c's shorter rational
function, worked out in f64, missed one in 600 (at 1.4 ns), and the f64
one costs half a ns more.

**`household` on the GPU (2026-10-18).** `LivesIn` is a tree of one
level, people in households: the GPU reads a person's household through
the edge in their slot (`source`, `target` of the tree, pieces of
`entityKeys` elements) and the sum of `bottom up` is a `for` over the
people whose `+=` into `pressure` (an `i64`) is atomic. A day of a
million people 9.2 ms on one core and the GPU, 8.5 on four threads
(13.6 and 11.8 before), `household` 0.04 ms where it took 2.0; the same
857,886 infected, and 32,718 with tracing (100,000 people, 6.6 ms a day
on four threads and the GPU). `test/Integration/Inputs/gpu_tree.ent`
moves people between houses on the CPU between frames.

**`tally` on the GPU, and no row after the last (2026-10-18).** The
model's `pair` wrote the row after the last person of `Order` (8 MB, the
rows `lay` writes on the GPU), which the copies took for a write of all
of it: back to the CPU and to the GPU again, every day. A write of a row
of a buffer outside a `for` is still a write of the whole piece to the
placement (what a range of the row would need: the row worked out where
the copies are placed); the model does without the row instead. A day
of a million people 7.9 ms on one core and the GPU, 7.6 on four threads.

**What a kernel touches is the GPU's (2026-10-18).** With `moves`,
`trace` and `told` run only where they do something (`run_if Run.log`,
`run_if Measures.telling`), the epidemic on four threads and the GPU
ended after a day with nobody ill in one run of two: every unique, the
archetypes' counts with them, was 0. The page of the CPU's world that
holds them had come back as the GPU's copy of it, while the CPU waited
for the stream: `workplace`'s kernel checked the other end of a tie
against its archetype's count, and read that count from the CPU's world
through its address. This driver has `Addressing Mode: HMM`, under which
a kernel reaches the CPU's memory and does not fault; and the page it
reads may move to the GPU and back. Counts were the CPU's
(`WorldAccess::count` of the GPU's copy gave the CPU's), loaded where the
lookup stands, which is in the kernel. Now, in a loop for the GPU, the
counts are the GPU's copy's, and they are a piece (the archetypes'
counts at the world's start, with the entity table: what lookups read
and spawns and destroys write), so they come to the GPU where the CPU
changed them. Three things keep it so:

- the lowering checks of itself that nothing in a loop for the GPU uses
  the CPU's world, or a view of it (`checkGpuLoops`: an internal error);
  `gpu.test` builds the epidemic with its systems on the GPU, which that
  check failed before;
- `ENT_GPU_PROTECT` runs every kernel with the CPU's world out of reach
  (`mprotect`) and fails, saying which kernel, where one touches it;
  `gpu.test` runs the GPU programs so;
- every copy between the CPU's memory and the GPU's goes through pinned
  staging memory of the runtime (`stage`, and what came back is put in
  place once the stream is done): the driver never moves the world's
  pages, and a copy to the GPU no longer waits for the stream.

Found by keeping, for each call of devices/gpu.c, the value of one
unique before and after it, in memory, said at the end (each probe that
wrote or waited hid it); then by `mprotect`. Without the GPU's counts:
11 of 20 runs; since, none of 40.

**`workplace`: few rows, long loops (2026-10-18).** `ENT_GPU_TIMES` gave
`workplace` 2.0 ms of a day's 5.1 on the GPU, from 0.03 ms on the first
day to 7.1 at the peak: it goes with how many are infectious, and those
are few among the rows its kernel goes through. A warp of 32 rows where
a handful pass the filter goes through the longest of their loops over
ties, the others idle. Three changes:

- *Compacting* (`compactGpuLoops`, LowerToLoops.cpp): a loop for the GPU
  whose body is `scf.if`s around a `for` or `while`, with nothing but
  reads before each test, is a loop that lists the rows that pass (an
  atomic count at the front of scratch array 15 of devices/gpu.c, and
  each row after it), and a loop over that list with the body (as many
  rows as the first, those after the count doing nothing). The rows
  that do the work are next to one another in a warp. It writes nothing
  but the list; the body sends atomically and writes its own row, so
  the order of the list does not matter. `chance` 650 to 430 us;
  `workplace` alone not, the next two are what it waited on.
- *Whether a tie was met, first*: the draw needs only the tie's key, so
  the other end's `Quarantine` is looked up only for the ties met that
  day (about half): `workplace` 2.0 to 1.8 ms.
- *A tie is two edges* (city.ent `wire`): one from each end, with one
  key. Going along a relation backwards (`(o)-[w: R]->(p)`) reads the
  index of the edges into `p` and then, for each, the edge and its
  source: two reads to far places a tie. Now `workplace` and `trace` go
  only forward, and the tie count of a person is their edges. The draws
  are the tie's key's, as before, and the hazard is whole numbers: the
  same 857,886 infected for a million people, and 32,718 with tracing
  for 100,000. `WorksWith` needs twice the capacity (22 a person).
  `workplace` 1.8 to 0.49 ms a day (three launches of 164 us).

A day of a million people 3.9 ms on one core and the GPU and on four
threads and the GPU (5.1 and 4.9 before); the GPU busy 342 ms in all
where it was 509. Measured in `bench/epidemic/RESULTS.md`.

**beecs, many colonies at once (2026-10-18).** A honeybee colony
(`examples/beecs`) is some 8,500 squadrons at most a round, a round a
dozen loops: on the GPU each a launch of microseconds and the copies
around it, slower than the CPU (two of its systems on the GPU made a
year 1.34 s where it took 0.91). `examples/beecs/many.ent` runs many
colonies side by side, each round's loops over the squadrons of all of
them, with nothing in a round that the GPU cannot do: places for the
squadrons that never move (a dead one's is taken by a new one), whole
numbers counted where fractions were summed, lists as running sums.
100 colonies of single-bee squadrons (2.5 million places) take 11.5 s
a year on the GPU, 186 s on one core; the same to the bit. What it
found:

- *The first pass of a running sum read what the body had not set.*
  On many cores and on the GPU a `for` with a running sum runs twice:
  a copy that works out only the values summed, and the body. The copy
  kept what the values are made of and dropped the rest, also the
  body's sets of its own fields; so where the body set a field and then
  read it to decide what it adds (`a.now = ..`, later `if a.now ==
  ..`), the copy read the field as it was before the `for`. Now such a
  field is a local of the copy (`localizeOwnFields`: an alloca that
  starts as the field, which the copy's sets and reads go to). Found as
  `fly` on the GPU giving 14 dancers where the CPU gave 38;
  `test/Integration/Inputs/running.ent` has such a field now, which
  gave `marks 0` on all cores and on the GPU without it.
- *Scans in chunks of 16 on the GPU*, not 64 (`runningChunk`): a turn
  of a level goes through its chunk's rows one after another, and the
  turns next to it read the rows after its own, which the GPU reads
  together where they are close. Two scans of 2.5 million rows a round
  took a third less.
- *Kernels of one name told apart.* Each kernel is a module of its own,
  and those a schedule makes of fused systems are all `round_kernel`:
  `ENT_GPU_TIMES` numbers them (`round_kernel#2`, ..) in the order they
  are first launched; and the name table had filled with the first 256
  launches (`mgpuModuleGetFunction` is asked at every launch), so later
  kernels had no name.

A round is then memory: five passes over all places, about 250 bytes a
place, some 2.7 ms a round for 2.5 million, at the card's 190 GB/s.

**A running sum that counts is bits on the GPU (2026-10-18).** A sum of
`+= 1` under an `if` (a place among those that have something) kept a
value for every row of the `for`, 4 bytes, which the scan read and
wrote again at every level: with two of them, `many.ent`'s lists cost
some 40 bytes a place a round, dead places too, 1 ms of its 2.7. Now
the first pass sets the row's bit in a word of 32 rows (an atomic `or`),
a pass counts each word's bits, the scan goes over the words' counts (a
32nd of the rows), and the body's place is its word's place and the
bits below its own (`llvm.intr.ctpop`). The lists' passes 0.25 ms a
round; 100 colonies a year 8.9 s where it was 11.5. In
`test/Integration/Inputs/running.ent` such sums of a few rows and of
70,000, the same on one core, on all and on the GPU.

**beecs's `fly`, and which loops are compacted (2026-10-18).** What
every flier worked out alike for its patch is the patch's, once a round
(`round_start`): a trip's kilometres (an `f64` division a flier, a
tenth of `fly`), the chance to give the patch up (1 / efficiency,
`decide`), the colony's honey as a share of decent honey; and the table
of the chances that a dance leads so many or fewer (`Chances`, made by
the same steps as `poisson_of`, which a dancer only compares with its
draw). The same lines to the byte. `fly` 525 to 400 us a round,
`decide` 327 to 303. Gathering what `fly` looks up of a patch into one
component (one lookup, not four) gained nothing, nor did reading the
colony through a tree (`-[Of]->(d: Day)`) instead of `Day(s.home)`: the
pass that lists who the filters let through then read the colony of
every place.

That pass was the other cost. A loop for the GPU was compacted where
its guards wrote nothing; but where what decides them is a lookup
(`if let done = Day(s.home).done`), the pass works it out for every row
and the body again. Now only guards that read the row's own columns
(`readsOwnRow`: loads at the row, and what is the same for all rows)
are kept for the list: `chance` in the epidemic still is; `many.ent`'s
loops are listed by their `where` alone, a cheap pass. 100 colonies a
year 8.17 to 7.70 s (CPU 8, the others busy). Not compacting them at
all would be 7.25: some 40% of the places pass, and the list costs its
pass, about 40 us, more than going through all of them saves. (A count
for each block of 256 rows instead of one for all made the pass three
times slower: the GPU adds a warp's 32 to one address as one, which it
cannot see for an address worked out from the row.)

The output of `test/Integration/Inputs/gpu_math.ent` was one line of
127 bytes, a byte more than a `Print` holds: what the test compared of
the f32 sum was its first ten digits. It is a line for each now, and a
`Print`'s line is a `text` (of any length, devices/text.ent): nothing is
cut off. Giving a text field its block (`ent.text.own`, and
`ent.text.drop` giving one back) no longer counts as an effect outside
component access, which made every system that printed keep its place
among all others: devices/text.c may run them on any thread at once.

With it `pair` is `today()` (where today's rows are), `lay()` (a `for`
over the people: their stubs, running sums of them and of the rows, and
the rows of `Order` and `Index`) and `pair()` (the order of the stubs);
`lay` runs on the GPU, and `chance` reads `Order` and `Index` where
`lay` left them. A running sum that adds the same number for every
entity from the body itself (`Laying.rows += 1`) is the row's place
times it: no pass of its own. Measured in `bench/epidemic/RESULTS.md`
(2026-10-17, later still).

**The hazard in whole numbers.** `Hazard.left` and `Household.pressure`
are `i64`s counting 2^-32ths (`fn hazard` in `city.ent`): each amount is
made a whole number where it is sent (times a power of two, then cut
towards nought: the same on every machine), and the sum of a day's is the
same in any order. Checked: the curves against the reference agree for
the baseline and a lockdown; with tracing one measure (days at home for
one infected) is 0.45 apart for seeds 1 to 20 where 0.43 is allowed, as
it is with the hazard as an `f32` to the digit (it was so before), and
within for seeds 21 to 40 and 41 to 60; one core and all give the same
days; the city's and the illness's numbers hold; the integration tests'
days are the same. A hundred days of a million people infect 857,886
both ways. On the CPU it costs 3% a day on one core and 11% on four
threads (178.7 against 172.9 ms, 68.1 against 61.2, on the slow cores:
`chance` sends 8 bytes where it sent 4), and 60 MB.

## A `for ... by` on the GPU (2026-10-23)

A `for` that goes by a field, whose body only reads and gives each
entity's rows to buffers (each `+=` in the body itself, not under an `if`
or in a loop: the form the CPU runs on many threads, docs/indexes.md step
1), runs on the GPU in a `gpu system` (`lowerOrderedOnGpu`):

- the entities are listed by a kernel for each archetype, each with its
  key and, in place of its id, its archetype's place among those the
  `for` goes over and its row (no row moves while it runs, so the visit
  needs no entity table);
- a radix sort of the GPU's puts them in order: the least key and the
  most found first (a kernel, each piece's put in at once with atomic
  `min` and `max`, read back), a pass for each byte up to the highest of
  the most less the least (keys of both signs differ in every byte as
  they are; 2026-10-25: it was each byte of it that is not nought, which
  left keys out of order where they carry across one that is), each a
  kernel that counts each tile's keys by their byte, a running sum over
  all the counts byte value by byte value and tile by tile (the
  running sums' own scan, `emitRunningScan`), and a kernel that puts each
  tile's keys where those say, in their order: a stable sort, so the
  order the CPU's gives. 2026-10-26: a tile is 4096 keys, a block of 256
  threads to each (a `gpu.launch` of its own, with arrays in the block's
  shared memory); the block counts its tile atomically in shared memory,
  and puts its keys a row of 256 at a time, a key to a thread: in a
  warp those of a byte value find each other (`match.sync`) and each
  goes after those of the lanes below it, the warps' counts go into a
  running sum of each byte value in shared memory, a thread to a value.
  It had been a thread to each piece of 256 keys, writing to places all
  over;
- each entity is then visited at its place in the order, writing its
  rows at the place times the rows it gives, after those its buffers
  had. The buffers' counts are the CPU's, as archetypes' are: the system
  reads them and writes them on the CPU, and asks whether there is room
  (`placeGpuCopies` copies such a count to the CPU before the run and
  marks it the CPU's after; the rows are the GPU's). The GPU's copy of
  the count is given the new one too, for the `for`s after it in the
  same system (`Inputs/gpu_given_read.ent`, 2026-10-25).

Checked in `test/Integration/gpu.test` (`Inputs/gpu_by.ent`, 301,000
entities of two archetypes, keys with many the same and below nought):
the same rows in the same order as on the CPU, with `--parallel` too, 40
runs, and with `ENT_GPU_PROTECT`. On the RTX 4050 (on battery) the sort
and visit of those take about 1.05 ms a frame on the GPU: two passes,
each about 230 us putting keys in place (to places all over), 60 us
counting; the visit 110 us. (First 8 ms: all eight passes, the least
and most found by one thread, the counts cleared in the counting
kernel.)

## Rows given to buffers on the GPU (2026-10-23)

`Marks += { .. }` in a `for` of a `gpu system` (at most once for an
entity, as anywhere: not from a loop of the body) puts its rows in the
order of the entities, as the CPU does (append by append, archetype by
archetype, row by row), on the GPU (`placeAppendedOnGpu`): the loop
notes each row's in its places, as on the CPU; then a kernel turns the
marks into ones and noughts, the running sums' scan makes each row's
place among those given, its whole read back, and a kernel puts each
row given at its place after those the buffer had. The buffer's count
is the CPU's, as with `for ... by` (above): copied to the CPU before the
run, asked whether there is room, marked the CPU's after; and given to
the GPU's copy, for the `for`s after in the same system.
`Inputs/gpu_given.ent` (`gpu.test`): 201,000 entities of two
archetypes, every one and those under an `if`, two `for`s into one
buffer, and a `gpu` system after that reads the rows and how many
there are: the same as the CPU, 40 runs, and with `ENT_GPU_PROTECT`.

## Memory a turn allocates, and `nearest k` (2026-10-25)

`nearest k` keeps the k nearest so far in a list the body of the `for`
allocates (`memref.alloc`, let go of at its end). In a kernel that was
an allocation from the GPU's heap in every turn: a million points' one
nearest each took 354 ms there, against 24 on the CPU's cores. Before a
loop for the GPU is made a kernel (`scratchForAllocations`), each such
allocation whose size is the same for every turn (worked out before the
loop, or in the turn of what is: `max(k, 0)`, moved before it) and that
is only read and written by its place becomes the turn's part of an
array in the GPU's memory made before the loop, as many places for each
turn (scratch slots from 32, kept from run to run as the running sums'
are). The same million points: 11.8 ms. `Inputs/gpu_nearest.ent`
(`gpu.test`): the k nearest in a grid and in a Morton index, k a
unique's, the same as on the CPU, with no `malloc` in the kernels.

## Windows counted, not each row claimed (2026-10-26)

Each entity's window of a buffer had each of its rows say whose it is
(an atomic exchange a row, its old value compared at once), so that two
windows over a row are found. On the GPU each such exchange is waited
for before the next: filling the LJ melt's lists (32,000 atoms, some 80
rows each) took 57 ms, counting them with the same walk 8. Where the
`for` does not also read the buffer by its name (then the owners are
needed: see takeWindowsFirst), each window now adds 1 to a count at its
start and takes 1 at its end (scratch slots 40 to 47, zeroed before the
query); after it a scan of the counts gives how many windows each row
is in, and a row in two stops the program, as before. The lists are
filled in 8.6 ms. `Inputs/gpu_window_clash.ent` (`gpu.test`): windows
of their own on the first day, overlapping on the second, which stops
the program on the GPU as on the CPU.

## A k-d tree made on the GPU (2026-10-26)

`ent.kdtree` in a `gpu system` (the k-d tree's `N_update_gpu`, chosen
as a Morton index's is) is lowered to loops for the GPU
(lowerKdTreeOnGpu): each side's list sorted by the GPU's radix sort
(emitGpuRadixSort, which a `for ... by` uses too), then at each level
the cells' boxes and children by a turn to a cell, and every place of
the lists at once: its cell (kept from level to level), whether its row
is below the cell's median, one running sum over all the lists of how
many below come before it, and so where it goes, below from the cell's
first place, the others from its median. Its arrays are scratch slots
from 48 (there are now 64). A million points in three sides: made in
about 8 ms, against 35 on all cores. `Inputs/kdtree_gpu.ent`
(`gpu.test`).

## Clustered indexes made on the GPU (2026-10-26)

A clustered index's form for the GPU ends with `ent.cluster`, which
lowerClusters lowers for the GPU where it is in a `gpu system`: each
entity's new row from the index's `who`, then every column of the
archetype and its ids through the scratch columns, the entity table
following (moveRows with its loops for the GPU), and each place its row;
every row moved, where the CPU's form first looks whether any does.
EntIndexes chooses it where the program has no relation (a tree that
keeps where its entities are is sorted again on the CPU). The k nearest
of a million points on the GPU: 5 to 25% faster clustered (bench/
RESULTS.md).
