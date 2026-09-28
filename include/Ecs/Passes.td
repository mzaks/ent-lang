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
    - `ecs.stage` is dissolved into its calls, or, with
      `parallel-stages`, a stage of two or more runs becomes an
      `omp.parallel` region with one `omp.section` per run. Parallel
      stages assume that the host passes columns that do not alias.
    - Components and archetypes are erased.
  }];
  let options = [
    Option<"parallelStages", "parallel-stages", "bool", /*default=*/"false",
           "Run the systems of a stage in parallel with OpenMP sections">,
  ];
  let dependentDialects = [
    "::mlir::arith::ArithDialect",
    "::mlir::func::FuncDialect",
    "::mlir::memref::MemRefDialect",
    "::mlir::omp::OpenMPDialect",
    "::mlir::scf::SCFDialect",
  ];
}

def EcsPrintAccess : Pass<"ecs-print-access", "ModuleOp"> {
  let summary = "Report the columns each system reads and writes";
  let description = [{
    Emits one remark per system listing the columns
    (`Archetype.Component.field`) it reads and writes, as derived from its
    `ecs.get` and `ecs.set` ops, and points at the op that makes a system
    opaque, if any. Meant for tests and for inspecting the analysis that
    `--ecs-schedule` relies on.
  }];
}

def EcsSchedule : Pass<"ecs-schedule", "ModuleOp"> {
  let summary = "Group the runs of each schedule into parallel stages";
  let description = [{
    Two runs conflict if one writes a column the other reads or writes, or
    if either system has effects outside component access. Each run is
    placed in the earliest stage after every earlier run it conflicts with,
    so runs within a stage commute and program order between conflicting
    runs is kept.

    An op in the schedule body with memory effects acts as a barrier:
    runs before it are staged before it, runs after it after it. Ops
    without effects stay where they are. Schedules that already contain
    stages are left unchanged.
  }];
  let options = [
    Option<"explain", "explain", "bool", /*default=*/"false",
           "Emit a remark on each run that has to wait, naming the run it "
           "waits for and the conflict">,
  ];
}

#endif // ECS_PASSES
