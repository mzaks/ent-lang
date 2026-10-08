#ifndef ENT_PASSES
#define ENT_PASSES

include "mlir/Pass/PassBase.td"

def EntLowerToLoops : Pass<"ent-lower-to-loops", "ModuleOp"> {
  let summary = "Lower systems and schedules to functions over column arrays";
  let description = [{
    Replaces the ECS program by plain functions over the world's storage.

    The world is one arena whose layout is fixed at compile time from the
    archetypes' components and capacities (see `WorldLayout`): one i64
    entity count per archetype, then one column per field at a static
    offset. Every lowered system and schedule takes its own parameters
    followed by the arena (`memref<Nxi8>`), and reads columns through
    statically shaped `memref.view`s. `ent-translate --ent-to-c-header`
    emits the matching C API for hosts.

    - `ent.system` becomes a private `func.func`. Each `ent.query` becomes
      one loop over every archetype that contains all of the query's
      components; `ent.get` and `ent.set` become `memref.load` and
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
    - `ent.schedule` becomes a public `func.func` with
      `llvm.emit_c_interface`; each `ent.run` becomes a `func.call`.
    - `ent.stage` is dissolved into its calls, or, with
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
    Option<"parallelMinEvents", "parallel-min-events", "int64_t",
           /*default=*/"16384",
           "Pending event-log entries from which a reactive query walks its "
           "logs' segments in parallel (with parallel-entities)">,
    Option<"parallelMinLevel", "parallel-min-level", "int64_t",
           /*default=*/"32768",
           "Entities of one depth of a sorted tree from which a cascading "
           "query visits the depth in parallel (with parallel-entities)">,
    Option<"fuseSystems", "fuse-systems", "bool", /*default=*/"false",
           "Inline the systems of each schedule and fuse their queries into "
           "one loop per archetype">,
    Option<"directApplies", "direct-applies", "bool", /*default=*/"true",
           "Combine an apply whose field nothing else in its query touches "
           "as the loop visits the entities, where the loop never runs in "
           "parallel, instead of buffering it until the query ends (same "
           "result)">,
    Option<"explain", "explain", "bool", /*default=*/"false",
           "Emit remarks on lowering decisions, such as reactive queries "
           "that scan every entity instead of walking event logs">,
  ];
  let dependentDialects = [
    "::mlir::arith::ArithDialect",
    "::mlir::cf::ControlFlowDialect",
    "::mlir::func::FuncDialect",
    "::mlir::LLVM::LLVMDialect",
    "::mlir::memref::MemRefDialect",
    "::mlir::omp::OpenMPDialect",
    "::mlir::scf::SCFDialect",
  ];
}

def EntInferArchetypes : Pass<"ent-infer-archetypes", "ModuleOp"> {
  let summary = "Derive the archetypes that spawns by components need";
  let description = [{
    Gives every `ent.spawn (@A, @B)(...)` an archetype and adds the
    archetypes this needs, marked `inferred` (see `inferArchetypes`): one per
    distinct set of spawned components, holding as optional the components
    adds and removes reach, sized by the smallest capacity among its
    required components. The other passes and `ent-translate` run it
    themselves; on its own it shows what they infer.
  }];
}

def EntOmpNowait : Pass<"ent-omp-nowait"> {
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

def EntLowerViews : Pass<"ent-lower-views", "::mlir::func::FuncOp"> {
  let summary = "Read and write the world's columns by their addresses";
  let description = [{
    After `--ent-lower-to-loops` a column is a `memref.view` of the world's
    memory, and what is read and written of it a `memref.load` or
    `memref.store` through that view. A view is a memref of its own: a
    record of five values once it is lowered, made and taken apart for
    every column a function touches, which is most of what a program with
    many archetypes is lowered to.

    This pass gives each such load and store the address it needs instead
    (`llvm.getelementptr` from the memory's own address, `llvm.load`,
    `llvm.store`), and drops the views nothing else uses. It changes
    nothing in what the program does; run it last before the conversion
    to the LLVM dialect.
  }];
  let dependentDialects = [
    "::mlir::LLVM::LLVMDialect", "::mlir::arith::ArithDialect",
    "::mlir::memref::MemRefDialect"
  ];
}

def EntPrintAccess : Pass<"ent-print-access", "ModuleOp"> {
  let summary = "Report the columns each system reads and writes";
  let description = [{
    Emits one remark per system listing the columns
    (`Archetype.Component.field`) it reads and writes, as derived from its
    `ent.get` and `ent.set` ops, and points at the op that makes a system
    opaque, if any. Meant for tests and for inspecting the analysis that
    `--ent-schedule` relies on.
  }];
}

def EntSchedule : Pass<"ent-schedule", "ModuleOp"> {
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

#endif // ENT_PASSES
