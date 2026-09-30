#ifndef ECS_PASSES
#define ECS_PASSES

include "mlir/Pass/PassBase.td"

def EcsLowerToLoops : Pass<"ecs-lower-to-loops", "ModuleOp"> {
  let summary = "Lower systems and schedules to functions over column arrays";
  let description = [{
    Replaces the ECS program by plain functions over the world's storage.

    The world is one arena whose layout is fixed at compile time from the
    archetypes' components and capacities (see `WorldLayout`): one i64
    entity count per archetype, then one column per field at a static
    offset. Every lowered system and schedule takes its own parameters
    followed by the arena (`memref<Nxi8>`), and reads columns through
    statically shaped `memref.view`s. `ecs-translate --ecs-to-c-header`
    emits the matching C API for hosts.

    - `ecs.system` becomes a private `func.func`. Each `ecs.query` becomes
      one loop over every archetype that contains all of the query's
      components; `ecs.get` and `ecs.set` become `memref.load` and
      `memref.store` on the matched archetype's column.
    - With `parallel-entities`, a query whose body only computes and
      touches its own entity may become an `scf.parallel`, ready for
      `--convert-scf-to-openmp`. An archetype whose capacity is below
      `parallel-min-entities` always gets a sequential loop; otherwise the
      count is checked at run time and the parallel loop runs only from
      that many entities on.
    - With `fuse-systems`, every maximal sequence of runs of systems
      without other effects is inlined into the schedule as one loop per
      archetype, holding every matching query body in program order.
      Stages are dissolved first; unused system functions remain for
      `--symbol-dce`.
    - `ecs.schedule` becomes a public `func.func` with
      `llvm.emit_c_interface`; each `ecs.run` becomes a `func.call`.
    - `ecs.stage` is dissolved into its calls, or, with
      `parallel-stages`, a stage of two or more runs becomes an
      `omp.parallel` region with one `omp.section` per run.
    - Components and archetypes are erased.
  }];
  let options = [
    Option<"parallelStages", "parallel-stages", "bool", /*default=*/"false",
           "Run the systems of a stage in parallel with OpenMP sections">,
    Option<"parallelEntities", "parallel-entities", "bool",
           /*default=*/"false",
           "Emit entity loops as scf.parallel where the body is entity-local">,
    Option<"parallelMinEntities", "parallel-min-entities", "int64_t",
           /*default=*/"1000000",
           "Entity count from which a parallel entity loop runs; on an M4 "
           "Max with default libomp the fused example pays off between 1e5 "
           "and 1e6 entities">,
    Option<"fuseSystems", "fuse-systems", "bool", /*default=*/"false",
           "Inline the systems of each schedule and fuse their queries into "
           "one loop per archetype">,
  ];
  let dependentDialects = [
    "::mlir::arith::ArithDialect",
    "::mlir::cf::ControlFlowDialect",
    "::mlir::func::FuncDialect",
    "::mlir::memref::MemRefDialect",
    "::mlir::omp::OpenMPDialect",
    "::mlir::scf::SCFDialect",
  ];
}

def EcsOmpNowait : Pass<"ecs-omp-nowait"> {
  let summary = "Drop the barrier after a parallel region's last loop";
  let description = [{
    A work-sharing loop (`omp.wsloop`) or `omp.sections` ends with a
    barrier unless it is `nowait`; so does every `omp.parallel` region.
    `--convert-scf-to-openmp` turns an `scf.parallel` into a parallel
    region holding nothing but a work-sharing loop, which then waits at two
    barriers back to back, where clang emits one for `parallel for`. With
    the OpenMP runtime's defaults, where waiting threads sleep, the second
    barrier costs about as much as the fork itself (see bench/RESULTS.md).

    This pass marks a work-sharing loop or sections construct `nowait`
    when it is the last op of its parallel region, since the region's own
    barrier follows. Constructs with reductions or private values are left
    alone. Run it after `--convert-scf-to-openmp`.
  }];
  let dependentDialects = ["::mlir::omp::OpenMPDialect"];
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
