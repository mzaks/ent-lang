# Systems on the GPU: design of G1

Status: step 1 (the marker and its checks) built; the rest is design. G0 (`bench/gpu/`, results in
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
- calling `fn`s of the program: compiled for the GPU like the body;
- arithmetic on every number type. `f64` is allowed; on consumer cards
  it runs at 1/32 to 1/64 of `f32`, which `--explain` says.

Errors, each naming the construct and what it waits for:

| In a `for` of a `gpu system` | Why not yet | When |
|---|---|---|
| `Health(e).hp -= ...`, `inner.w += ...` (applies), `Hits += 1` (accumulates) | combined at the commit point in a fixed order: needs ordered reductions on the GPU | G2 |
| `Marks += { ... }` (rows given to a buffer) | rows in the order of the entities: needs an ordered compaction | G2 |
| `C(other).f` (lookups) | needs the table from id to row on the GPU | G2 |
| `extern fn`, including `math`'s `sin`, `sqrt` | the C runs on the CPU; the GPU's own functions round differently from the machine's library, which breaks the same-results rule | G2 |
| a `for` over other entities inside the `for` | fine on a GPU (it is its best case), but needs the `old` copy and the inner loop's order kept | G2 |
| arrows, edges, `top down`, `bottom up` | edges and trees are kept in tables the GPU does not have | later |
| `on changed ...` (reactive) | event logs and stamps | later |
| `spawn`, `destroy`, `add`/`remove` of an `apart` component | change which rows an archetype holds; the compaction runs on the CPU | later |
| `by`, `in turn by` | one entity after another: no parallel form | never |
| a table (`table`), a text (`text[N]`, `text`) | not on the GPU yet | later |
| a fn given the entities of a component | it reads other entities | G2, with lookups |
| `extern proc` | the outside world | never |

An extern system cannot be `gpu`.

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

The world's arena is allocated as managed memory (`cuMemAllocManaged`)
in a `--gpu` build: one address for the CPU and the GPU, the same offsets,
so kernels use the views the lowering already makes, and the C host,
extern systems, saving and loading read and write the world as they do
now. The driver moves pages to whichever side touches them. That makes
correctness depend on one thing only, waiting; where the pages are is
then a matter of speed, which the compiler helps with hints.

**Waiting.** Kernels are launched without waiting, one after another on
one stream that belongs to the world. The CPU waits for the stream
(`ent_gpu_wait`) before a run that conflicts with a GPU run since the
last wait: it reads a column a kernel writes, or writes one a kernel reads
or writes. "Conflicts" is the relation `--ent-schedule` already uses to
form stages (`SystemAccess::conflictWith`, `include/Ent/Access.h`), so a
CPU run that touches nothing the GPU touches (`tick`, writing `Clock`)
runs while the kernels do. Also:

- an extern system that declares nothing conflicts with everything, as it
  does for stages: it waits, which covers `save_world` and `load_world`;
- a run's `run_if` reads only uniques, which no kernel writes in G1;
- a schedule called by a C host waits before it returns (H1: the host may
  read the world between calls); one called by `main` does not, since
  `main` reads only uniques. So frames of a `main` loop follow each other
  on the stream and the CPU waits only where it needs a column.

Whether there is anything to wait for is a flag in the world's host part,
so a wait the compiler cannot rule out costs a branch when there is none.

**Hints.** Before a GPU run, the columns it touches are prefetched to the
GPU (`cuMemPrefetchAsync`, the rows in use); before a CPU run touching a
column a GPU run touched, to the CPU. A byte per column in the world's
host part says where it was last sent, so a column already there is not
sent again. A spawn on the CPU then costs the pages of the new rows, not
the column.

**Layout.** In a `--gpu` build, the columns of archetypes a GPU system
visits start at a 64 KB boundary (the driver moves managed memory in such
pieces), so a page the CPU writes every frame (counts, uniques) never
holds a column's first rows. The arena grows by at most 64 KB a column.
G0's 16 us floor may partly be this sharing; G1 measures it.

A buffer a GPU system reads is a column like any other: its rows in use
are prefetched and its writes on the CPU send it back, by the same bytes.

Later, where managed memory is slow or missing (Windows, some AMD
parts), the same waits and hints can drive a second arena on the GPU with
explicit copies instead; G1 does not need it.

## Runtime: `devices/gpu.c`

The functions MLIR's lowering calls (`mgpuModuleLoadJIT`,
`mgpuModuleGetFunction`, `mgpuLaunchKernel`, `mgpuStream*`, unload), as
in G0's `bench/gpu/gpu_runtime.c`, and ent's own: `ent_gpu_alloc` (the
arena), `ent_gpu_wait`, `ent_gpu_prefetch`. One stream per world:
`mgpuStreamCreate` gives the world's, and destroy and synchronize after
a launch are dropped (`gpu-async-region`, or ent's own launch op if that
is simpler), since waiting is the compiler's (above). MLIR's ROCm
wrappers have the same `mgpu*` names, so another vendor is another file.

It is a device in the sense of `devices/`: C, linked by `tools/ent` with
`-lcuda` in a `--gpu` build, not imported by programs. The generated
header's `ent_world_create` calls `ent_gpu_alloc` instead of
`posix_memalign` when the world is built for a GPU.

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
   Checked as IR by lit tests.
3. **Runtime and tools.** `devices/gpu.c`, `tools/ent --gpu`, the GPU test
   mode; `bench/gpu/run.py` goes through `tools/ent` instead of its own
   pipeline.
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
