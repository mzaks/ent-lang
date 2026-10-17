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
- going along a relation's edges from either end (`for (p)-[w: R]->(o:
  mut C)`, `for (o)-[w: R]->(p)`): the edges' fields, written too
  (`mut w`), the other end's components, and what is sent to it (G2: the
  sorted table, the offsets of each entity's edges and the index by
  target are pieces, copied as many edges as there are). Not a tree's;
- calling `fn`s of the program: compiled for the GPU like the body;
- arithmetic on every number type. `f64` is allowed; on consumer cards
  it runs at 1/32 to 1/64 of `f32`, which `--explain` says.

Errors, each naming the construct and what it waits for:

| In a `for` of a `gpu system` | Why not yet | When |
|---|---|---|
| sending or accumulating a fraction (`f32`, `f64`), or sending with `=` | combined at the commit point in the order of the rows, which the GPU does not keep: needs the sends sorted by target and combined in that order on the GPU (keep such a system on the CPU) | later |
| sending or accumulating in a loop of the body | sent from loops goes on lists the CPU keeps | later |
| sending to a field the query also reads or writes | what is sent lands when the query ends; on the GPU it lands as it is sent | never |
| `Marks += { ... }` (rows given to a buffer) | rows in the order of the entities: needs an ordered compaction | G2 |
| `extern fn`, including `math`'s `sin`, `sqrt` | the C runs on the CPU; the GPU's own functions round differently from the machine's library, which breaks the same-results rule | G2 |
| a `for` over other entities inside the `for` | fine on a GPU (it is its best case), but needs the `old` copy and the inner loop's order kept | G2 |
| arrows up a tree, a tree's edges, `top down`, `bottom up` | trees are kept in tables the GPU does not have | later |
| `connect`, `disconnect` | the relation is sorted again on the CPU | later |
| `on changed ...` (reactive) | event logs and stamps | later |
| `spawn`, `destroy`, `add`/`remove` of an `apart` component | change which rows an archetype holds; the compaction runs on the CPU | later |
| `by`, `in turn by` | one entity after another: no parallel form | never |
| a table (`table`), a text (`text[N]`, `text`) | not on the GPU yet | later |
| a fn given the entities of a component | it reads other entities through a function of the CPU's, given the world | later |
| `extern proc` | the outside world | never |
| writing or sending to a component a reactive query reacts to, or to the field a tree is ordered by | its events (stamps), and the tree's order, are kept on the CPU | later |

An extern system cannot be `gpu`. The code of a `gpu system` around its
`for`s reads and writes uniques and computes; what it would do to
entities or buffers (spawn, `Marks = []`, a lookup) goes in a system of
its own, so that every run touches the world's columns from one side.

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
- `math` on the GPU (G2): the same results on both sides need the same
  implementation on both. Candidates: correctly rounded functions (LLVM's
  libc has them and builds for NVPTX), or ent-lang implementations in
  `math.ent` used on both sides. Either changes the CPU's results from the
  machine's library's.
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
component). `chance` (sends from a loop of the body) is next. They are
also what a world in pieces over several GPUs or machines needs (edges
between the pieces, sends from many places combined in one order).

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
