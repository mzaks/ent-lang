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

Run the toy simulation: lower it, link it with its C host and execute it.

```sh
build/bin/ecs-opt examples/integrate.mlir --ecs-lower-to-loops \
    --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts \
  | /opt/homebrew/opt/llvm/bin/mlir-translate --mlir-to-llvmir -o /tmp/integrate.ll
/opt/homebrew/opt/llvm/bin/clang -O2 -Wno-override-module /tmp/integrate.ll \
    examples/host/integrate_main.c -o /tmp/integrate && /tmp/integrate
```

## The dialect today

- `ecs.component @Position (x: f32, y: f32)`: scalar fields, abstract layout.
- `ecs.archetype @Body (@Position, @Velocity, @Mass)`: a table that entities
  are stored in. The set of archetypes is closed, so every query is matched
  to its archetypes at compile time.
- `ecs.system @s(%params) reads [...] writes [...] { ... }`: declared access;
  `writes` implies read.
- `ecs.query (%p: !ecs.ref<@Position, mut>, ...) { ... }`: body runs once per
  matching entity; refs exist only as query arguments.
- `ecs.get` / `ecs.set`: field access through a ref; `set` needs `mut`.
- `ecs.schedule @frame(%params) { ecs.run @s(...) }`: program order is the
  semantic order.

The verifier checks that every query stays inside its system's declared
access, that no query binds a component twice, that refs are only used by
`ecs.get` and `ecs.set`, and that field names and types match the component
declarations.

## Lowering

`--ecs-lower-to-loops` turns systems into private functions and schedules into
public ones with a C interface (`_mlir_ciface_<schedule>`). Each query becomes
one `scf.for` per matching archetype, and field access becomes `memref.load`
and `memref.store` on that archetype's columns.

The world is passed explicitly. Per archetype, in declaration order, it is an
entity count (`index`) and one `memref<?xT>` column per field, ordered by the
archetype's components and then by each component's fields. Every lowered
function takes its own parameters followed by the whole world;
`examples/host/integrate_main.c` shows the host side.

## Milestones

- [x] **M0**: the dialect parses, prints and verifies.
- [x] **M1**: lower the toy simulation to loops and run it (from a C host
      rather than `mlir-runner`, which cannot call a schedule from an MLIR
      `main` before it is lowered).
- [ ] **M2**: access analysis and a scheduling pass (compile-time parallelism).
- [ ] **M3**: layout choice (SoA/AoSoA), system fusion, benchmarks.
- [ ] Later: structural changes (spawn/despawn), change detection, relations,
      surface syntax.
