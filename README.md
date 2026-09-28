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

## The dialect today

- `ecs.component @Position (x: f32, y: f32)`: scalar fields, abstract layout.
- `ecs.system @s(%params) reads [...] writes [...] { ... }`: declared access;
  `writes` implies read.
- `ecs.query (%p: !ecs.ref<@Position, mut>, ...) { ... }`: body runs once per
  matching entity; refs exist only as query arguments.
- `ecs.get` / `ecs.set`: field access through a ref; `set` needs `mut`.
- `ecs.schedule @frame(%params) { ecs.run @s(...) }`: program order is the
  semantic order.

The verifier checks that every query stays inside its system's declared
access, that no query binds a component twice, and that field names and types
match the component declarations.

## Milestones

- [x] **M0**: the dialect parses, prints and verifies.
- [ ] **M1**: lower the toy simulation to loops and run it with `mlir-runner`.
- [ ] **M2**: access analysis and a scheduling pass (compile-time parallelism).
- [ ] **M3**: layout choice (SoA/AoSoA), system fusion, benchmarks.
- [ ] Later: structural changes (spawn/despawn), change detection, relations,
      surface syntax.
