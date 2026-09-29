# ecs-lang

A research language built on the Entity Component System pattern. The
semantics are developed first as an MLIR dialect (`ecs`), written by hand and
transformed by passes; a surface syntax comes later.

## Build

Requires Homebrew LLVM (tested with 22.1.8, which ships MLIR, FileCheck and
`mlir-runner`) and CMake. `lit` lives in a local venv.

```sh
python3 -m venv .venv && .venv/bin/pip install lit
cmake -S . -B build -DMLIR_DIR=/opt/homebrew/opt/llvm/lib/cmake/mlir \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
cmake --build build --target check-ecs
build/bin/ecs-opt examples/integrate.mlir
```

Run the toy simulation: generate the world's C header, lower the program
with fused, parallel entity loops, and link it with its C host and the
OpenMP runtime. Drop `parallel-entities`, `--convert-scf-to-openmp` and the
OpenMP flags for a sequential build.

```sh
LLVM=/opt/homebrew/opt/llvm
build/bin/ecs-translate --ecs-to-c-header examples/integrate.mlir \
    -o /tmp/integrate_world.h
build/bin/ecs-opt examples/integrate.mlir \
    "--ecs-lower-to-loops=fuse-systems=1 parallel-entities=1" --symbol-dce \
    --convert-scf-to-openmp --convert-scf-to-cf --convert-to-llvm \
    --reconcile-unrealized-casts \
  | $LLVM/bin/mlir-translate --mlir-to-llvmir -o /tmp/integrate.ll
$LLVM/bin/clang -O2 -Wno-override-module -I/tmp /tmp/integrate.ll \
    examples/host/integrate_main.c -L$LLVM/lib -lomp -Wl,-rpath,$LLVM/lib \
    -o /tmp/integrate && /tmp/integrate
```

Inspect the analysis behind the schedule:

```sh
build/bin/ecs-opt examples/integrate.mlir --ecs-print-access -o /dev/null
build/bin/ecs-opt examples/integrate.mlir --ecs-schedule=explain=1
```

## The dialect today

- `ecs.component @Position (x: f32, y: f32)`: scalar fields, abstract layout.
- `ecs.archetype @Body (@Position, @Velocity, @Mass) capacity 100000`: a
  table that entities are stored in, with a hard upper bound on its size.
  The set of archetypes is closed, so every query is matched to its
  archetypes at compile time.
- `ecs.system @s(%params) reads [...] writes [...] { ... }`: declared access;
  `writes` implies read.
- `ecs.query (%p: !ecs.ref<@Position, mut>, ...) { ... }`: body runs once per
  matching entity; refs exist only as query arguments.
- `ecs.get` / `ecs.set`: field access through a ref; `set` needs `mut`.
- `ecs.schedule @frame(%params) { ecs.run @s(...) }`: program order is the
  semantic order.
- `ecs.stage { ecs.run ... }`: runs that commute and may execute in
  parallel; stages execute in order.

The verifier checks that every query stays inside its system's declared
access, that no query binds a component twice, that refs are only used by
`ecs.get` and `ecs.set`, and that field names and types match the component
declarations.

## Access analysis and scheduling

A system's access is computed per column, `Archetype.Component.field`, from
the `ecs.get` and `ecs.set` ops it contains, in every archetype its queries
match. Declared `reads`/`writes` remain the contract the verifier enforces,
but the analysis is finer: two systems that both declare `writes [@Velocity]`
do not conflict if one only touches `Body.Velocity.dy` and the other only
`Particle.Velocity.dx`, and binding a component to select archetypes is not
a read. Any other op with memory effects (a call, say) makes a system
opaque, and opaque systems conflict with everything.

`--ecs-schedule` puts each run into the earliest stage after every earlier
run it conflicts with. In the example, gravity, wind and decay share the
first stage and integrate follows. Ops with effects in a schedule body act
as barriers.

## Lowering

`--ecs-lower-to-loops` turns systems into private functions and schedules into
public ones with a C interface (`_mlir_ciface_<schedule>`). Each query becomes
one `scf.for` per matching archetype, and field access becomes `memref.load`
and `memref.store` on that archetype's columns. Stages dissolve into calls,
or with `parallel-stages=1` a stage of several runs becomes an `omp.parallel`
region with one `omp.section` per run.

## World storage

The language owns the world's storage. From the archetypes' components and
capacities, the compiler lays out the whole world as one arena: an i64
entity count per archetype, then one column per field at a fixed offset.
Every column starts on a 64-byte boundary, 17 cache lines past the end of
the previous one; columns packed from a page-aligned base would start at
the same cache set, which cost a single core 7-8% (see
`bench/RESULTS.md`). Lowered functions take the arena after their own
parameters and read columns through statically shaped views.

`ecs-translate --ecs-to-c-header` emits the C API for hosts:
`ecs_world_create`/`ecs_world_destroy`, per archetype a capacity, a count
and a checked `set_count`, typed column accessors such as
`ecs_Body_Position_x(world)`, and one entry point per schedule, such as
`ecs_frame(world, dt)`. `examples/host/integrate_main.c` shows the host
side. Creating a world only touches the counts, so capacity costs address
space, not memory, until columns are written. Parallel stages and loops
assume nothing else writes the arena while a schedule runs.

## Fusion and entity parallelism

Every access in a query goes through a ref to the current entity, and
different archetypes share no columns. Two consequences the lowering uses:

- `fuse-systems=1` inlines the systems of a schedule and emits one loop per
  archetype holding every matching query body in program order. This is
  legal for any sequence of systems without other effects, conflicting or
  not: running all bodies for one entity before the next gives the same
  result as running each query to completion.
- `parallel-entities=1` emits entity-local query loops as `scf.parallel`;
  `--convert-scf-to-openmp` turns them into OpenMP work-sharing loops. A
  parallel loop only pays for its fork beyond some size, so an archetype
  whose capacity is below `parallel-min-entities` (default 1e6, the
  measured crossover on an M4 Max) always gets a sequential loop, and
  above it the count decides at run time.

## Benchmarks

`bench/run.py` builds every variant of the example (per-system loops,
parallel stages, parallel entity loops, fused, fused with parallel entity
loops, and hand-written C with and without `restrict`), runs each
measurement in its own process, round-robin over sizes and variants, and
checks that all variants produce the same checksum.

```sh
python3 bench/run.py                    # OpenMP runtime defaults
python3 bench/run.py --blocktime 200    # keep OpenMP workers spinning
```

Homebrew's `libomp` defaults to `KMP_BLOCKTIME=0` with a passive wait
policy, so workers sleep after every parallel region; an empty region costs
about 35 us with 12 threads on an M4 Max, and about 1.7 us with
`KMP_BLOCKTIME=200`. Check that the machine is otherwise idle before
trusting a run. Results, with the conditions they were taken under, are in
`bench/RESULTS.md`.

## Milestones

- [x] **M0**: the dialect parses, prints and verifies.
- [x] **M1**: lower the toy simulation to loops and run it (from a C host
      rather than `mlir-runner`, which cannot call a schedule from an MLIR
      `main` before it is lowered).
- [x] **M2**: access analysis and a scheduling pass (compile-time parallelism).
- [ ] **M3**: layout choice (SoA/AoSoA), system fusion, benchmarks.
- [ ] Later: structural changes (spawn/despawn), change detection, relations,
      surface syntax.
