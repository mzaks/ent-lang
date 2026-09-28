#ifndef ECS_PASSES
#define ECS_PASSES

include "mlir/Pass/PassBase.td"

def EcsLowerToLoops : Pass<"ecs-lower-to-loops", "ModuleOp"> {
  let summary = "Lower systems and schedules to functions over column arrays";
  let description = [{
    Replaces the ECS program by plain functions over the world's storage.

    World ABI: for every `ecs.archetype`, in declaration order, the world
    holds an entity count (`index`) followed by one `memref<?xT>` column per
    field, ordered by the archetype's component list and then by each
    component's field list. Every lowered system and schedule takes its own
    parameters followed by the whole world.

    - `ecs.system` becomes a private `func.func`. Each `ecs.query` becomes
      one `scf.for` over every archetype that contains all of the query's
      components; `ecs.get` and `ecs.set` become `memref.load` and
      `memref.store` on the matched archetype's column.
    - `ecs.schedule` becomes a public `func.func` with
      `llvm.emit_c_interface`, so a host program can call
      `_mlir_ciface_<name>`; each `ecs.run` becomes a `func.call`.
    - `ecs.stage` is dissolved into its calls, in program order.
    - Components and archetypes are erased.
  }];
  let dependentDialects = [
    "::mlir::arith::ArithDialect",
    "::mlir::func::FuncDialect",
    "::mlir::memref::MemRefDialect",
    "::mlir::scf::SCFDialect",
  ];
}

#endif // ECS_PASSES
